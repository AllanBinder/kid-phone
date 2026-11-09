#include "usb_audio.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "soc/soc.h"

#include "app_config.h"

static const char *TAG = "usb_audio";

/* USB host FIFO split, in 4-byte lines (200 total on the ESP32-S3).
 * Layout 0 is the original split; layout 1 (Advanced > Handset
 * troubleshooting) gives the earpiece more room at the mic's expense
 * (RX 56 lines still holds a 192 B mic packet plus status words). */
typedef struct {
    uint16_t nptx, ptx, rx;
} fifo_layout_t;
static const fifo_layout_t k_layouts[HS_LAYOUT_COUNT] = {
    {16, 96, 88},
    {16, 128, 56},
};
#define PERIODIC_OUT_MPS_LIMIT (k_layouts[s_hs_layout].ptx * 4)

/* Handset troubleshooting settings, read from NVS once at start. */
#define HS_NVS_NS "kidphone"
static uint8_t s_hs_layout;
static uint8_t s_hs_flags;

/* Last handset seen, for the setup page. */
static char s_hs_name[UAC_STR_DESC_MAX_LENGTH + 1];
static uint16_t s_hs_vid, s_hs_pid;

/* Rates in order of preference. All are integer multiples of 8 kHz, so the
 * telephone path (mode B) can use a plain integer decimator. */
static const uint32_t k_pref_rates[] = {48000, 16000, 32000, 24000, 8000};
#define N_PREF_RATES (sizeof(k_pref_rates) / sizeof(k_pref_rates[0]))

typedef enum {
    EVT_DRIVER,
    EVT_DEVICE,
} evt_kind_t;

typedef struct {
    evt_kind_t kind;
    union {
        struct {
            uint8_t addr;
            uint8_t iface_num;
            uac_host_driver_event_t event;
        } drv;
        struct {
            uac_host_device_handle_t handle;
            uac_host_device_event_t event;
        } dev;
    };
} evt_t;

static QueueHandle_t s_evt_q;
static SemaphoreHandle_t s_lock;
static ua_state_t s_state;

bool usb_audio_lock(TickType_t timeout)
{
    return xSemaphoreTake(s_lock, timeout) == pdTRUE;
}

void usb_audio_unlock(void)
{
    xSemaphoreGive(s_lock);
}

ua_state_t *usb_audio_state(void)
{
    return &s_state;
}

/* ---------------------------------------------------------------------------
 * Callbacks: run in the UAC driver task. Only enqueue, never block.
 * RX_DONE / TX_DONE fire on nearly every transfer; the engine polls the
 * driver ring buffers instead, so those are dropped here.
 * ------------------------------------------------------------------------- */

static void uac_driver_cb(uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t event, void *arg)
{
    (void)arg;
    evt_t e = {.kind = EVT_DRIVER, .drv = {.addr = addr, .iface_num = iface_num, .event = event}};
    xQueueSend(s_evt_q, &e, 0);
}

static void uac_device_cb(uac_host_device_handle_t handle, const uac_host_device_event_t event, void *arg)
{
    (void)arg;
    if (event == UAC_HOST_DEVICE_EVENT_RX_DONE || event == UAC_HOST_DEVICE_EVENT_TX_DONE) {
        return;
    }
    evt_t e = {.kind = EVT_DEVICE, .dev = {.handle = handle, .event = event}};
    xQueueSend(s_evt_q, &e, 0);
}

/* ---------------------------------------------------------------------------
 * Descriptor logging
 * ------------------------------------------------------------------------- */

static void wstr_to_ascii(const wchar_t *w, char *out, size_t out_len)
{
    size_t i = 0;
    for (; i + 1 < out_len && i < UAC_STR_DESC_MAX_LENGTH && w[i]; i++) {
        out[i] = (w[i] >= 0x20 && w[i] < 0x7f) ? (char)w[i] : '?';
    }
    out[i] = '\0';
}

static void log_alt(uint8_t alt, const uac_host_dev_alt_param_t *p)
{
    char rates[96];
    int n = 0;
    if (p->sample_freq_type > 0) {
        for (int i = 0; i < p->sample_freq_type && i < UAC_FREQ_NUM_MAX; i++) {
            n += snprintf(rates + n, sizeof(rates) - n, "%s%" PRIu32, i ? ", " : "", p->sample_freq[i]);
            if (n >= (int)sizeof(rates)) {
                break;
            }
        }
        if (p->sample_freq_type > UAC_FREQ_NUM_MAX) {
            snprintf(rates + n, sizeof(rates) - n, " (+%d more not stored)", p->sample_freq_type - UAC_FREQ_NUM_MAX);
        }
    } else {
        snprintf(rates, sizeof(rates), "continuous %" PRIu32 " .. %" PRIu32, p->sample_freq_lower, p->sample_freq_upper);
    }
    ESP_LOGI(TAG, "    alt %u: format=%s, %u ch, %u-bit in %u-byte subframe, rates Hz: %s",
             alt, p->format == 1 ? "PCM" : "non-PCM", p->channels, p->bit_resolution, p->subframe_size, rates);
}

static void log_controls(uac_host_device_handle_t h, const char *what)
{
    int16_t vol_db = 0;
    uint8_t vol_pct = 0;
    esp_err_t r_db = uac_host_device_get_volume_db(h, &vol_db);
    esp_err_t r_pct = uac_host_device_get_volume(h, &vol_pct);
    /* No GET_CUR mute here (removed in 0.8.1, GitHub issues #1 and #2). It
     * only fed a log line, and some chips (SPACETOUCH 12d1:3a07) never answer
     * it on a feature unit without a mute control. usb_host_uac 1.5.0 skips
     * SET mute when the mute channel map is empty but still sends the GET, so
     * the control transfer times out, EP0 cannot be recovered, and every later
     * control request fails: the earpiece then never starts ("Unable to set
     * Interface alternate"). */

    if (r_db == ESP_OK) {
        ESP_LOGI(TAG, "    %s volume control: YES, now %.2f dB (%u%% of driver range)",
                 what, vol_db / 256.0f, r_pct == ESP_OK ? vol_pct : 0);
    } else {
        ESP_LOGI(TAG, "    %s volume control: no (%s)", what, esp_err_to_name(r_db));
    }
}

/* ---------------------------------------------------------------------------
 * Format selection
 * ------------------------------------------------------------------------- */

static bool alt_has_rate(const uac_host_dev_alt_param_t *p, uint32_t rate)
{
    if (p->sample_freq_type > 0) {
        for (int i = 0; i < p->sample_freq_type && i < UAC_FREQ_NUM_MAX; i++) {
            if (p->sample_freq[i] == rate) {
                return true;
            }
        }
        return false;
    }
    return rate >= p->sample_freq_lower && rate <= p->sample_freq_upper;
}

static int bits_score(const uac_host_dev_alt_param_t *p)
{
    if (p->subframe_size == 2 && p->bit_resolution == 16) {
        return 4;
    }
    if (p->subframe_size == 3) {
        return 3;
    }
    if (p->subframe_size == 4) {
        return 2;
    }
    if (p->subframe_size == 2) {
        return 1;
    }
    return 0; /* 8-bit or odd: not supported by this firmware */
}

/*
 * Pick channels, bit depth and rate for one stream.
 * prefer_rate (if nonzero) is the other direction's rate, so both sides can
 * run at the same rate and mode A stays a straight copy.
 */
static bool pick_format(uac_host_device_handle_t h, uint8_t n_alts, bool is_mic, uint32_t prefer_rate,
                        ua_stream_t *out)
{
    int best = -1;
    for (uint8_t alt = 1; alt <= n_alts; alt++) {
        uac_host_dev_alt_param_t p;
        if (uac_host_get_device_alt_param(h, alt, &p) != ESP_OK || p.format != 1) {
            continue;
        }
        int bs = bits_score(&p);
        if (bs == 0 || p.channels == 0 || p.channels > 2) {
            continue;
        }
        /* Candidate rates: the other side's rate, then the preference list,
         * then (discrete only) whatever the device lists. */
        uint32_t cands[1 + N_PREF_RATES + UAC_FREQ_NUM_MAX];
        int nc = 0;
        if (prefer_rate) {
            cands[nc++] = prefer_rate;
        }
        for (size_t i = 0; i < N_PREF_RATES; i++) {
            cands[nc++] = k_pref_rates[i];
        }
        if (p.sample_freq_type > 0) {
            for (int i = 0; i < p.sample_freq_type && i < UAC_FREQ_NUM_MAX; i++) {
                cands[nc++] = p.sample_freq[i];
            }
        } else if (p.sample_freq_lower) {
            cands[nc++] = p.sample_freq_lower;
        }
        for (int c = 0; c < nc; c++) {
            uint32_t r = cands[c];
            if (r == 0 || r % 100 != 0 || !alt_has_rate(&p, r)) {
                continue;
            }
            int rate_score = 100 - c; /* earlier candidate wins */
            int ch_score = is_mic ? (p.channels == 1 ? 1 : 0) : 1; /* stereo out is fine */
            int score = rate_score * 100 + bs * 10 + ch_score;
            if (score > best) {
                best = score;
                out->rate = r;
                out->channels = p.channels;
                out->subframe = p.subframe_size;
                out->bits = p.bit_resolution;
            }
            break; /* first usable candidate is the best for this alt */
        }
    }
    return best >= 0;
}

/* ---------------------------------------------------------------------------
 * Open / close
 * ------------------------------------------------------------------------- */

/* Some handsets declare an earpiece (isoc OUT) MPS bigger than the periodic
 * TX FIFO (e.g. 600 B), and the host refuses to claim the interface ("EP MPS
 * (600) exceeds supported limit (384)"). The real stream is far smaller
 * (48 kHz 16-bit stereo = 192 B per 1 ms packet), and an OUT endpoint
 * accepts any packet up to its MPS, so lowering the host's copy of
 * wMaxPacketSize to the FIFO limit is safe. It has to happen before
 * uac_host_device_open(), which caches the MPS, and the claim parses the
 * same heap copy. IN endpoints are never touched: the device may send up to
 * its MPS. Uses a short-lived client because the UAC driver's is private. */
static void clamp_client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)msg;
    (void)arg;
}

static void clamp_earpiece_mps(uint8_t addr)
{
    const usb_host_client_config_t ccfg = {
        .is_synchronous = false,
        .max_num_event_msg = 2,
        .async = {.client_event_callback = clamp_client_cb, .callback_arg = NULL},
    };
    usb_host_client_handle_t client = NULL;
    if (usb_host_client_register(&ccfg, &client) != ESP_OK) {
        ESP_LOGW(TAG, "  MPS check: client register failed");
        return;
    }
    usb_device_handle_t dev = NULL;
    if (usb_host_device_open(client, addr, &dev) == ESP_OK) {
        const usb_config_desc_t *cfg = NULL;
        if (usb_host_get_active_config_descriptor(dev, &cfg) == ESP_OK && cfg) {
            uint8_t *p = (uint8_t *)cfg; /* host's heap copy, writable */
            for (uint16_t off = 0; off + 2 <= cfg->wTotalLength && p[off] >= 2; off += p[off]) {
                if (p[off + 1] != USB_B_DESCRIPTOR_TYPE_ENDPOINT || off + sizeof(usb_ep_desc_t) > cfg->wTotalLength) {
                    continue;
                }
                usb_ep_desc_t *ep = (usb_ep_desc_t *)(p + off);
                bool isoc_out = (ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_ISOC &&
                                !(ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK);
                uint16_t mps = USB_EP_DESC_GET_MPS(ep);
                if (isoc_out && mps > PERIODIC_OUT_MPS_LIMIT) {
                    ep->wMaxPacketSize = (ep->wMaxPacketSize & ~0x7FF) | PERIODIC_OUT_MPS_LIMIT;
                    ESP_LOGW(TAG, "  earpiece EP 0x%02X declares MPS %u, clamped to %u (FIFO limit)",
                             ep->bEndpointAddress, mps, PERIODIC_OUT_MPS_LIMIT);
                }
            }
        }
        usb_host_device_close(client, dev);
    } else {
        ESP_LOGW(TAG, "  MPS check: device open failed");
    }
    usb_host_client_deregister(client);
}

static void open_stream(uint8_t addr, uint8_t iface_num, bool is_mic)
{
    const char *what = is_mic ? "MIC (RX, device->host)" : "EARPIECE (TX, host->device)";
    ESP_LOGI(TAG, "==== UAC interface found: %s, usb addr %u, interface %u ====", what, addr, iface_num);
    if (is_mic && (s_hs_flags & HS_FLAG_MIC_OFF)) {
        ESP_LOGW(TAG, "  mic left off (Handset troubleshooting: speaker only test)");
        return;
    }

    /* Driver ring sized in blocks of 48 kHz stereo 16-bit (the likely
     * format); the exact format is not known until the alt settings are read.
     * A 24/32-bit format still fits at least two blocks. */
    const uint32_t blk = 48000 / 1000 * APP_BLOCK_MS * 2 * 2;
    const uint32_t nblk = is_mic ? APP_RX_RING_BLOCKS : APP_TX_RING_BLOCKS;
    const uac_host_device_config_t cfg = {
        .addr = addr,
        .iface_num = iface_num,
        .buffer_size = blk * nblk,
        .buffer_threshold = blk,
        .callback = uac_device_cb,
        .callback_arg = NULL,
    };
    if (!is_mic) {
        clamp_earpiece_mps(addr);
    }
    uac_host_device_handle_t h = NULL;
    const bool verbose = APP_USB_DEBUG || (s_hs_flags & HS_FLAG_VERBOSE);
    /* The driver logs the volume range at debug level during open. */
    if (verbose) {
        esp_log_level_set("uac-host", ESP_LOG_DEBUG);
    }
    esp_err_t err = uac_host_device_open(&cfg, &h);
    if (verbose && !(s_hs_flags & HS_FLAG_VERBOSE)) {
        esp_log_level_set("uac-host", ESP_LOG_INFO);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  open failed: %s", esp_err_to_name(err));
        return;
    }

    uac_host_dev_info_t info;
    if (uac_host_get_device_info(h, &info) == ESP_OK) {
        char man[UAC_STR_DESC_MAX_LENGTH + 1], prod[UAC_STR_DESC_MAX_LENGTH + 1], ser[UAC_STR_DESC_MAX_LENGTH + 1];
        wstr_to_ascii(info.iManufacturer, man, sizeof(man));
        wstr_to_ascii(info.iProduct, prod, sizeof(prod));
        wstr_to_ascii(info.iSerialNumber, ser, sizeof(ser));
        ESP_LOGI(TAG, "  device: VID:PID %04x:%04x, manufacturer \"%s\", product \"%s\", serial \"%s\"",
                 info.VID, info.PID, man, prod, ser);
        if (usb_audio_lock(portMAX_DELAY)) {
            strlcpy(s_hs_name, prod, sizeof(s_hs_name));
            s_hs_vid = info.VID;
            s_hs_pid = info.PID;
            usb_audio_unlock();
        }
        ESP_LOGI(TAG, "  %u alternate setting(s) with audio:", info.iface_alt_num);
        for (uint8_t alt = 1; alt <= info.iface_alt_num; alt++) {
            uac_host_dev_alt_param_t p;
            if (uac_host_get_device_alt_param(h, alt, &p) == ESP_OK) {
                log_alt(alt, &p);
            }
        }
    } else {
        ESP_LOGW(TAG, "  could not read device info");
        info.iface_alt_num = 0;
    }

    /* Prefer the other direction's rate if it is already running. */
    uint32_t prefer = 0;
    if (usb_audio_lock(portMAX_DELAY)) {
        const ua_stream_t *other = is_mic ? &s_state.spk : &s_state.mic;
        prefer = other->active ? other->rate : 0;
        usb_audio_unlock();
    }

    ua_stream_t fmt = {0};
    if (!pick_format(h, info.iface_alt_num, is_mic, prefer, &fmt)) {
        ESP_LOGE(TAG, "  no usable format (need PCM, 1-2 ch, 16/24/32-bit, rate multiple of 100 Hz)");
        uac_host_device_close(h);
        return;
    }
    ESP_LOGI(TAG, "  chosen: %" PRIu32 " Hz, %u ch, %u-bit (%u-byte subframe)%s",
             fmt.rate, fmt.channels, fmt.bits, fmt.subframe,
             (fmt.rate % 8000 == 0) ? "" : "  WARNING: not a multiple of 8 kHz, mode B unavailable");

    const uac_host_stream_config_t stm = {
        .channels = fmt.channels,
        .bit_resolution = fmt.bits,
        .sample_freq = fmt.rate,
        .flags = 0,
    };
    /* Handsets that declare an oversized OUT MPS (e.g. 600) need the host
     * descriptor clamp above; SET_INTERFACE can still flake once on a busy
     * bus, especially if the mic stream was already running. Retry a few
     * times before giving up on the earpiece. */
    err = ESP_FAIL;
    for (int try = 1; try <= 3; try++) {
        err = uac_host_device_start(h, &stm);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "  stream start try %d failed: %s", try, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(50 * try));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  stream start failed: %s", esp_err_to_name(err));
        uac_host_device_close(h);
        return;
    }

    /* Controls only work once the stream is active. */
    uint8_t want = is_mic ? APP_MIC_VOLUME_PCT : APP_SPK_VOLUME_PCT;
    if (!is_mic && (s_hs_flags & HS_FLAG_KEEP_VOL)) {
        ESP_LOGW(TAG, "  earpiece volume and mute left as the handset set them (Handset troubleshooting)");
    } else {
        esp_err_t rv = uac_host_device_set_volume(h, want);
        esp_err_t rm = uac_host_device_set_mute(h, false);
        ESP_LOGI(TAG, "  set volume %u%%: %s, unmute: %s", want, esp_err_to_name(rv), esp_err_to_name(rm));
    }
    log_controls(h, is_mic ? "mic" : "earpiece");

    fmt.handle = h;
    fmt.active = true;
    if (usb_audio_lock(portMAX_DELAY)) {
        if (is_mic) {
            s_state.mic = fmt;
        } else {
            s_state.spk = fmt;
        }
        s_state.generation++;
        usb_audio_unlock();
    }
    ESP_LOGI(TAG, "==== %s READY ====", is_mic ? "MIC" : "EARPIECE");
}

static void close_stream(uac_host_device_handle_t h)
{
    bool found = false;
    const char *what = "unknown interface";
    /* Take the lock so the engine is not mid read/write on this handle. */
    if (usb_audio_lock(portMAX_DELAY)) {
        if (s_state.mic.handle == h) {
            memset(&s_state.mic, 0, sizeof(s_state.mic));
            what = "MIC";
            found = true;
        } else if (s_state.spk.handle == h) {
            memset(&s_state.spk, 0, sizeof(s_state.spk));
            what = "EARPIECE";
            found = true;
        }
        s_state.generation++;
        usb_audio_unlock();
    }
    ESP_LOGW(TAG, "%s disconnected, closing", what);
    esp_err_t err = uac_host_device_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "  close returned %s", esp_err_to_name(err));
    }
    if (found) {
        ESP_LOGI(TAG, "waiting for handset (plug it into the native USB port)");
    }
}

/* ---------------------------------------------------------------------------
 * Tasks
 * ------------------------------------------------------------------------- */

#if APP_USB_DEBUG
/* Bring-up probe: raw DWC OTG host port register (HPRT, base 0x60080000 + 0x440).
 * Polled every 2 s, logged only when the value changes. */
static void port_probe_task(void *arg)
{
    (void)arg;
    uint32_t last = UINT32_MAX;
    while (1) {
        uint32_t h = REG_READ(0x60080440);
        if (h == last) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        last = h;
        ESP_LOGI(TAG, "HPRT=0x%08" PRIx32 " conn=%u enabled=%u power=%u linestate D+=%u D-=%u speed=%u",
                 h, (unsigned)(h & 1), (unsigned)((h >> 2) & 1), (unsigned)((h >> 12) & 1),
                 (unsigned)((h >> 10) & 1), (unsigned)((h >> 11) & 1), (unsigned)((h >> 17) & 3));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
#endif

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        /* 200-line (800 B) FIFO split by hand: the AB17X earpiece isoc OUT
         * endpoint has MPS 384, too big for the default bias presets.
         * RX 88 lines = 344 B IN MPS (mic), PTX 96 = 384 B (earpiece),
         * NPTX 16 = 64 B (control). Bigger earpiece MPS: clamp_earpiece_mps(). */
        .fifo_settings_custom = {
            .nptx_fifo_lines = k_layouts[s_hs_layout].nptx,
            .ptx_fifo_lines = k_layouts[s_hs_layout].ptx,
            .rx_fifo_lines = k_layouts[s_hs_layout].rx,
        },
    };
#if APP_USB_DEBUG
    /* Bring-up: show root port connect/enumeration activity */
    esp_log_level_set("HUB", ESP_LOG_DEBUG);
    esp_log_level_set("ENUM", ESP_LOG_DEBUG);
    esp_log_level_set("USBH", ESP_LOG_DEBUG);
    esp_log_level_set("USB HOST", ESP_LOG_DEBUG);
#endif
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    ESP_LOGI(TAG, "USB host installed (native port, GPIO 19/20), FIFO layout %u: RX %u, PTX %u, NPTX %u lines",
             s_hs_layout, k_layouts[s_hs_layout].rx, k_layouts[s_hs_layout].ptx, k_layouts[s_hs_layout].nptx);
    if (s_hs_flags & HS_FLAG_VERBOSE) {
        esp_log_level_set("uac-host", ESP_LOG_DEBUG);
        esp_log_level_set("USBH", ESP_LOG_DEBUG);
    }
#if APP_USB_DEBUG
    xTaskCreate(port_probe_task, "port_probe", 3072, NULL, 2, NULL);
#endif
    xTaskNotifyGive((TaskHandle_t)arg);
    while (1) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGD(TAG, "USB host: all devices freed");
        }
    }
}

static void control_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    const uac_host_driver_config_t uac_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_driver_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(uac_host_install(&uac_cfg));
    ESP_LOGI(TAG, "UAC class driver installed, waiting for handset (plug it into the native USB port)");

    /* Defer the mic (RX) until after the earpiece (TX) for the same device.
     * Opening the mic first leaves isoc IN running; on handsets with an
     * oversized OUT MPS (clamped to the PTX FIFO) the following earpiece
     * SET_INTERFACE then fails and the earpiece never starts. */
    bool pending_mic = false;
    uint8_t pending_mic_addr = 0;
    uint8_t pending_mic_iface = 0;
    TickType_t pending_mic_deadline = 0;

    evt_t e;
    while (1) {
        TickType_t wait = portMAX_DELAY;
        if (pending_mic) {
            TickType_t now = xTaskGetTickCount();
            wait = (now >= pending_mic_deadline) ? 0 : (pending_mic_deadline - now);
        }
        if (xQueueReceive(s_evt_q, &e, wait) != pdTRUE) {
            if (pending_mic) {
                ESP_LOGW(TAG, "earpiece did not appear; opening mic alone");
                open_stream(pending_mic_addr, pending_mic_iface, true);
                pending_mic = false;
            }
            continue;
        }
        if (e.kind == EVT_DRIVER) {
            switch (e.drv.event) {
            case UAC_HOST_DRIVER_EVENT_RX_CONNECTED: {
                bool spk_up = false;
                if (usb_audio_lock(pdMS_TO_TICKS(50))) {
                    spk_up = s_state.spk.active;
                    usb_audio_unlock();
                }
                if (spk_up) {
                    open_stream(e.drv.addr, e.drv.iface_num, true);
                } else {
                    pending_mic = true;
                    pending_mic_addr = e.drv.addr;
                    pending_mic_iface = e.drv.iface_num;
                    pending_mic_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
                    ESP_LOGI(TAG, "mic interface seen (addr %u iface %u); waiting for earpiece first",
                             pending_mic_addr, pending_mic_iface);
                }
                break;
            }
            case UAC_HOST_DRIVER_EVENT_TX_CONNECTED:
                open_stream(e.drv.addr, e.drv.iface_num, false);
                if (pending_mic && pending_mic_addr == e.drv.addr) {
                    open_stream(pending_mic_addr, pending_mic_iface, true);
                    pending_mic = false;
                }
                break;
            default:
                break;
            }
        } else {
            switch (e.dev.event) {
            case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
                close_stream(e.dev.handle);
                pending_mic = false;
                break;
            case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
                ESP_LOGW(TAG, "USB transfer error on %s", e.dev.handle == s_state.mic.handle ? "MIC" : "EARPIECE");
                break;
            default:
                break;
            }
        }
    }
}

static void hs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(HS_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "hs_layout", &v) == ESP_OK && v < HS_LAYOUT_COUNT) {
            s_hs_layout = v;
        }
        if (nvs_get_u8(h, "hs_flags", &v) == ESP_OK) {
            s_hs_flags = v & HS_FLAG_ALL;
        }
        nvs_close(h);
    }
    if (s_hs_layout || s_hs_flags) {
        ESP_LOGW(TAG, "handset troubleshooting: FIFO layout %u%s%s%s", s_hs_layout,
                 (s_hs_flags & HS_FLAG_MIC_OFF) ? ", mic off" : "",
                 (s_hs_flags & HS_FLAG_KEEP_VOL) ? ", earpiece volume untouched" : "",
                 (s_hs_flags & HS_FLAG_VERBOSE) ? ", verbose USB log" : "");
    }
}

void usb_audio_hs_get(uint8_t *layout, uint8_t *flags)
{
    *layout = s_hs_layout;
    *flags = s_hs_flags;
}

esp_err_t usb_audio_hs_save(uint8_t layout, uint8_t flags)
{
    if (layout >= HS_LAYOUT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(HS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "hs_layout", layout);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "hs_flags", flags & HS_FLAG_ALL);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool usb_audio_hs_device(char *name, size_t cap, uint16_t *vid, uint16_t *pid)
{
    if (!usb_audio_lock(pdMS_TO_TICKS(20))) {
        return false;
    }
    bool have = s_hs_vid || s_hs_pid;
    strlcpy(name, s_hs_name, cap);
    *vid = s_hs_vid;
    *pid = s_hs_pid;
    usb_audio_unlock();
    return have;
}

void usb_audio_start(void)
{
    hs_load();
    s_lock = xSemaphoreCreateMutex();
    s_evt_q = xQueueCreate(16, sizeof(evt_t));
    assert(s_lock && s_evt_q);

    TaskHandle_t ctrl = NULL;
    xTaskCreatePinnedToCore(control_task, "uac_ctrl", 6144, NULL, 4, &ctrl, 0);
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, (void *)ctrl, 10, NULL, 0);
}

bool usb_audio_handset_connected(void)
{
    if (!usb_audio_lock(pdMS_TO_TICKS(20))) {
        return false;
    }
    const ua_state_t *st = usb_audio_state();
    bool r = st->spk.active || st->mic.active;
    usb_audio_unlock();
    return r;
}
