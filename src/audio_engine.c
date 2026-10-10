#include "audio_engine.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "g711.h"
#include "modes.h"
#include "resampler.h"
#include "rtp.h"
#include "usb_audio.h"

static const char *TAG = "audio";

#define TEL_RATE        8000
#define MAX_RATE        48000
#define MAX_BLOCK       (MAX_RATE / 1000 * APP_BLOCK_MS)      /* frames per block at 48 kHz */
#define TEL_BLOCK       (TEL_RATE / 1000 * APP_BLOCK_MS)      /* 80 */
#define MIC_RING_CAP    (MAX_BLOCK * (APP_MIC_MAX_BLOCKS + 2))
#define RAW_BUF_BYTES   (MAX_BLOCK * 2 * 4 * 2)               /* two blocks, stereo, 32-bit */

/* ---------------------------------------------------------------------------
 * Mic jitter ring (mono int16 at the mic rate). Only the audio task touches it.
 * ------------------------------------------------------------------------- */
static int16_t s_mic_ring[MIC_RING_CAP];
static size_t s_mr_head, s_mr_count;

static void mic_ring_clear(void)
{
    s_mr_head = 0;
    s_mr_count = 0;
}

/* Returns the number of samples dropped (oldest first) to make room. */
static size_t mic_ring_push(const int16_t *x, size_t n, size_t cap)
{
    size_t dropped = 0;
    for (size_t i = 0; i < n; i++) {
        size_t tail = (s_mr_head + s_mr_count) % MIC_RING_CAP;
        s_mic_ring[tail] = x[i];
        if (s_mr_count < cap) {
            s_mr_count++;
        } else {
            s_mr_head = (s_mr_head + 1) % MIC_RING_CAP;
            dropped++;
        }
    }
    return dropped;
}

static size_t mic_ring_pop(int16_t *x, size_t n)
{
    if (n > s_mr_count) {
        n = s_mr_count;
    }
    for (size_t i = 0; i < n; i++) {
        x[i] = s_mic_ring[s_mr_head];
        s_mr_head = (s_mr_head + 1) % MIC_RING_CAP;
    }
    s_mr_count -= n;
    return n;
}

/* ---------------------------------------------------------------------------
 * Sample format conversion (USB little-endian PCM <-> mono int16)
 * ------------------------------------------------------------------------- */

static inline int32_t read_sample(const uint8_t *p, uint8_t subframe)
{
    switch (subframe) {
    case 2: return (int16_t)(p[0] | (p[1] << 8));
    case 3: return (int16_t)(p[1] | (p[2] << 8));          /* top 16 of 24 */
    case 4: return (int16_t)(p[2] | (p[3] << 8));          /* top 16 of 32 */
    default: return 0;
    }
}

static inline void write_sample(uint8_t *p, uint8_t subframe, int16_t v)
{
    switch (subframe) {
    case 2: p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; break;
    case 3: p[0] = 0; p[1] = v & 0xff; p[2] = (v >> 8) & 0xff; break;
    case 4: p[0] = 0; p[1] = 0; p[2] = v & 0xff; p[3] = (v >> 8) & 0xff; break;
    default: break;
    }
}

/* Interleaved device frames -> mono (channels averaged). */
static void unpack_mono(const uint8_t *raw, size_t frames, const ua_stream_t *s, int16_t *out)
{
    const size_t fsz = (size_t)s->channels * s->subframe;
    for (size_t i = 0; i < frames; i++) {
        const uint8_t *f = raw + i * fsz;
        int32_t acc = 0;
        for (int c = 0; c < s->channels; c++) {
            acc += read_sample(f + c * s->subframe, s->subframe);
        }
        out[i] = (int16_t)(acc / s->channels);
    }
}

/* Mono -> interleaved device frames (duplicated to every channel). */
static void pack_mono(const int16_t *in, size_t frames, const ua_stream_t *s, uint8_t *raw)
{
    const size_t fsz = (size_t)s->channels * s->subframe;
    for (size_t i = 0; i < frames; i++) {
        uint8_t *f = raw + i * fsz;
        for (int c = 0; c < s->channels; c++) {
            write_sample(f + c * s->subframe, s->subframe, in[i]);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Engine state
 * ------------------------------------------------------------------------- */

typedef struct {
    /* stream snapshot for this generation */
    uint32_t gen;
    ua_stream_t mic, spk;
    bool have_mic, have_spk;

    /* telephone path */
    bool tel_ok;           /* both rates are integer multiples of 8 kHz */
    resampler_t dec;       /* mic rate -> 8 kHz */
    resampler_t itp;       /* 8 kHz -> spk rate */

    /* dial tone oscillators */
    float ph1, ph2;
    uint32_t ring_pos;   /* samples into the ringback cadence */

    bool primed;           /* loopback has enough mic data buffered */
    app_mode_t last_mode;

    /* stats, reset every period */
    uint32_t frames_in, frames_out, underruns, overruns, tx_fail;
    int peak;
    int64_t next_stats_us;
} engine_t;

static engine_t E;

static uint8_t s_raw_in[RAW_BUF_BYTES];
static size_t s_raw_carry;                 /* bytes of a partial frame left in s_raw_in */
static uint8_t s_raw_out[MAX_BLOCK * 2 * 4];
static int16_t s_mono_in[RAW_BUF_BYTES / 2];
static int16_t s_blk_mic[MAX_BLOCK];
static int16_t s_blk_out[MAX_BLOCK];
static int16_t s_blk_tel[TEL_BLOCK];
static uint8_t s_blk_ulaw[TEL_BLOCK];

static void reset_paths(void)
{
    mic_ring_clear();
    s_raw_carry = 0;
    E.primed = false;
    if (E.tel_ok) {
        resampler_reset(&E.dec);
        resampler_reset(&E.itp);
    }
}

/* Called with the usb lock held when the generation changed. */
static void reconfigure(const ua_state_t *st)
{
    resampler_deinit(&E.dec);
    resampler_deinit(&E.itp);
    E.gen = st->generation;
    E.mic = st->mic;
    E.spk = st->spk;
    E.have_mic = st->mic.active;
    E.have_spk = st->spk.active;
    E.tel_ok = false;

    if (E.have_mic && E.have_spk && E.mic.rate % TEL_RATE == 0 && E.spk.rate % TEL_RATE == 0) {
        int rd = E.mic.rate / TEL_RATE;
        int ri = E.spk.rate / TEL_RATE;
        E.tel_ok = resampler_decim_init(&E.dec, rd, (float)E.mic.rate, APP_TEL_CUTOFF_HZ) &&
                   resampler_interp_init(&E.itp, ri, (float)E.spk.rate, APP_TEL_CUTOFF_HZ);
        if (!E.tel_ok) {
            ESP_LOGE(TAG, "resampler init failed (out of memory?)");
        }
    }
    reset_paths();

    if (E.have_mic || E.have_spk) {
        ESP_LOGI(TAG, "engine: mic %s, earpiece %s, telephone path %s, wideband %s",
                 E.have_mic ? "yes" : "NO", E.have_spk ? "yes" : "NO",
                 E.tel_ok ? "ready" : "unavailable",
                 (E.have_mic && E.have_spk && E.mic.rate == E.spk.rate) ? "direct copy"
                 : (E.tel_ok ? "rates differ, falls back to 8 kHz path" : "unavailable"));
    }
}

/* Pull everything the driver has for the mic into the jitter ring. */
static void drain_mic(void)
{
    const size_t fsz = (size_t)E.mic.channels * E.mic.subframe;
    const size_t cap = (size_t)E.mic.rate / 1000 * APP_BLOCK_MS * APP_MIC_MAX_BLOCKS;
    for (int pass = 0; pass < 4; pass++) {
        uint32_t got = 0;
        size_t want = (sizeof(s_raw_in) - s_raw_carry) / fsz * fsz;
        esp_err_t err = uac_host_device_read(E.mic.handle, s_raw_in + s_raw_carry, want, &got, 0);
        if (err != ESP_OK || got == 0) {
            return;
        }
        size_t total = s_raw_carry + got;
        size_t frames = total / fsz;
        unpack_mono(s_raw_in, frames, &E.mic, s_mono_in);
        s_raw_carry = total - frames * fsz;
        if (s_raw_carry) {
            memmove(s_raw_in, s_raw_in + frames * fsz, s_raw_carry);
        }
        for (size_t i = 0; i < frames; i++) {
            int a = abs(s_mono_in[i]);
            if (a > E.peak) {
                E.peak = a;
            }
        }
        E.frames_in += frames;
        size_t dropped = mic_ring_push(s_mono_in, frames, cap);
        if (dropped) {
            E.overruns++;
        }
        if (got < want) {
            return; /* driver ring is empty now */
        }
    }
}

/*
 * Take n mic samples for loopback. Waits for APP_MIC_PRIME_BLOCKS of data
 * after any starvation so a short gap does not turn into constant stutter.
 * Returns false (and zero-fills) when not enough data.
 */
static bool take_mic(int16_t *dst, size_t n)
{
    const size_t prime = (size_t)E.mic.rate / 1000 * APP_BLOCK_MS * APP_MIC_PRIME_BLOCKS;
    if (!E.primed) {
        if (s_mr_count < prime) {
            memset(dst, 0, n * sizeof(int16_t));
            return false;
        }
        E.primed = true;
    }
    size_t got = mic_ring_pop(dst, n);
    if (got < n) {
        memset(dst + got, 0, (n - got) * sizeof(int16_t));
        E.underruns++;
        E.primed = false;
        return false;
    }
    return true;
}

/* mic block -> 8 kHz -> (optional mu-law round trip) -> spk rate. */
static void telephone_path(const int16_t *mic, size_t mic_n, int16_t *out, bool use_ulaw)
{
    size_t n8 = resampler_decim_process(&E.dec, mic, mic_n, s_blk_tel);
    if (use_ulaw) {
        g711_ulaw_encode_buf(s_blk_tel, s_blk_ulaw, n8);
        g711_ulaw_decode_buf(s_blk_ulaw, s_blk_tel, n8);
    }
    resampler_interp_process(&E.itp, s_blk_tel, n8, out);
}

/*
 * Call: the mode B pipeline split in two halves around the network.
 *   mic -> decimate to 8 kHz -> mu-law -> RTP sender (stream buffer)
 *   jitter buffer -> mu-law decode -> interpolate to the earpiece rate
 * The jitter buffer returns silence until early media or the answer arrives.
 */
static void call_path(const int16_t *mic, size_t mic_n, int16_t *out)
{
    size_t n8 = resampler_decim_process(&E.dec, mic, mic_n, s_blk_tel);
    g711_ulaw_encode_buf(s_blk_tel, s_blk_ulaw, n8);
    rtp_audio_tx_push(s_blk_ulaw, n8);

    rtp_audio_rx_pull(s_blk_ulaw, TEL_BLOCK);
    g711_ulaw_decode_buf(s_blk_ulaw, s_blk_tel, TEL_BLOCK);
    resampler_interp_process(&E.itp, s_blk_tel, TEL_BLOCK, out);
}

static void dial_tone(int16_t *out, size_t n, uint32_t rate)
{
    const float two_pi = 6.28318530718f;
    const float d1 = two_pi * 350.0f / rate;
    const float d2 = two_pi * 440.0f / rate;
    const float a = APP_DIALTONE_AMPL * 32767.0f;
    for (size_t i = 0; i < n; i++) {
        out[i] = (int16_t)(a * (sinf(E.ph1) + sinf(E.ph2)));
        E.ph1 += d1;
        E.ph2 += d2;
        if (E.ph1 > two_pi) {
            E.ph1 -= two_pi;
        }
        if (E.ph2 > two_pi) {
            E.ph2 -= two_pi;
        }
    }
}

/* US ringback: 440 + 480 Hz, 2 s on, 4 s off. Starts at the top of a ring. */
static void ringback(int16_t *out, size_t n, uint32_t rate)
{
    const uint32_t on = rate * APP_RINGBACK_ON_MS / 1000;
    const uint32_t period = rate * (APP_RINGBACK_ON_MS + APP_RINGBACK_OFF_MS) / 1000;
    const float two_pi = 6.28318530718f;
    const float d1 = two_pi * 440.0f / rate;
    const float d2 = two_pi * 480.0f / rate;
    const float a = APP_RINGBACK_AMPL * 32767.0f;
    for (size_t i = 0; i < n; i++) {
        out[i] = E.ring_pos < on ? (int16_t)(a * (sinf(E.ph1) + sinf(E.ph2))) : 0;
        E.ph1 += d1;
        E.ph2 += d2;
        if (E.ph1 > two_pi) {
            E.ph1 -= two_pi;
        }
        if (E.ph2 > two_pi) {
            E.ph2 -= two_pi;
        }
        if (++E.ring_pos >= period) {
            E.ring_pos = 0;
        }
    }
}

static void produce_block(app_mode_t mode, size_t out_n)
{
    static app_mode_t prev = MODE_D_SILENCE;
    if (mode == MODE_RINGBACK && prev != MODE_RINGBACK) {
        E.ring_pos = 0;
    }
    prev = mode;

    const size_t mic_n = E.have_mic ? (size_t)E.mic.rate / 1000 * APP_BLOCK_MS : 0;

    switch (mode) {
    case MODE_A_WIDEBAND_LOOPBACK:
        if (!E.have_mic) {
            memset(s_blk_out, 0, out_n * sizeof(int16_t));
        } else if (E.mic.rate == E.spk.rate) {
            take_mic(s_blk_out, out_n);
        } else if (E.tel_ok) {
            take_mic(s_blk_mic, mic_n);
            telephone_path(s_blk_mic, mic_n, s_blk_out, false);
        } else {
            memset(s_blk_out, 0, out_n * sizeof(int16_t));
        }
        break;
    case MODE_B_TELEPHONE_LOOPBACK:
        if (E.tel_ok) {
            take_mic(s_blk_mic, mic_n);
            telephone_path(s_blk_mic, mic_n, s_blk_out, true);
        } else {
            memset(s_blk_out, 0, out_n * sizeof(int16_t));
        }
        break;
    case MODE_C_DIAL_TONE:
        dial_tone(s_blk_out, out_n, E.spk.rate);
        break;
    case MODE_RINGBACK:
        ringback(s_blk_out, out_n, E.spk.rate);
        break;
    case MODE_CALL:
        if (E.tel_ok) {
            take_mic(s_blk_mic, mic_n);
            call_path(s_blk_mic, mic_n, s_blk_out);
        } else {
            memset(s_blk_out, 0, out_n * sizeof(int16_t));
        }
        break;
    case MODE_D_SILENCE:
    default:
        memset(s_blk_out, 0, out_n * sizeof(int16_t));
        break;
    }
}

static void print_stats(app_mode_t mode)
{
    char mic[40] = "none";
    char spk[40] = "none";
    if (E.have_mic) {
        snprintf(mic, sizeof(mic), "%" PRIu32 "Hz/%uch/%ub", E.mic.rate, E.mic.channels, E.mic.bits);
    }
    if (E.have_spk) {
        snprintf(spk, sizeof(spk), "%" PRIu32 "Hz/%uch/%ub", E.spk.rate, E.spk.channels, E.spk.bits);
    }
    float dbfs = E.peak > 0 ? 20.0f * log10f(E.peak / 32768.0f) : -99.0f;
    ESP_LOGI(TAG, "stats 5s: mode %s | mic %s earpiece %s | in %" PRIu32 " out %" PRIu32
             " frames | underruns %" PRIu32 " overruns %" PRIu32 " txfail %" PRIu32
             " | mic peak %d (%.1f dBFS) | jitter buf %u",
             app_mode_short(mode), mic, spk, E.frames_in, E.frames_out,
             E.underruns, E.overruns, E.tx_fail, E.peak, dbfs, (unsigned)s_mr_count);
    E.frames_in = E.frames_out = E.underruns = E.overruns = E.tx_fail = 0;
    E.peak = 0;
}

static void audio_task(void *arg)
{
    (void)arg;
    E.gen = UINT32_MAX;
    E.last_mode = app_mode_get();
    E.next_stats_us = esp_timer_get_time() + APP_STATS_PERIOD_MS * 1000LL;

    while (1) {
        app_mode_t mode = app_mode_get();
        bool wrote = false;

        if (usb_audio_lock(pdMS_TO_TICKS(100))) {
            const ua_state_t *st = usb_audio_state();
            if (st->generation != E.gen) {
                reconfigure(st);
            }
            if (mode != E.last_mode) {
                ESP_LOGI(TAG, "audio mode %s -> %s", app_mode_short(E.last_mode), app_mode_short(mode));
                reset_paths();
                E.last_mode = mode;
            }

            if (E.have_mic) {
                drain_mic();
            }
            if (E.have_spk) {
                const size_t out_n = (size_t)E.spk.rate / 1000 * APP_BLOCK_MS;
                produce_block(mode, out_n);
                pack_mono(s_blk_out, out_n, &E.spk, s_raw_out);
                const size_t bytes = out_n * E.spk.channels * E.spk.subframe;
                /* Blocks until the driver ring has room: this is the pacing. */
                esp_err_t err = uac_host_device_write(E.spk.handle, s_raw_out, bytes,
                                                      pdMS_TO_TICKS(APP_BLOCK_MS * 5));
                if (err == ESP_OK) {
                    E.frames_out += out_n;
                    wrote = true;
                } else {
                    E.tx_fail++;
                }
            } else if (E.have_mic) {
                /* Mic only: keep the jitter ring from growing without bound. */
                mic_ring_clear();
            }
            usb_audio_unlock();
        }

        if (!wrote) {
            /* No earpiece (or write failed): pace with a sleep instead. */
            vTaskDelay(pdMS_TO_TICKS(APP_BLOCK_MS));
        }

        int64_t now = esp_timer_get_time();
        if (now >= E.next_stats_us) {
            E.next_stats_us = now + APP_STATS_PERIOD_MS * 1000LL;
            if (E.have_mic || E.have_spk) {
                print_stats(mode);
            } else {
                ESP_LOGI(TAG, "stats 5s: mode %s | no handset connected", app_mode_short(mode));
            }
        }
    }
}

void audio_engine_start(void)
{
    /* Core 1: away from the USB host and UAC driver tasks on core 0. */
    xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 6, NULL, 1);
}
