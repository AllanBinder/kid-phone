#include "rtp.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "app_config.h"

static const char *TAG = "rtp";

#define PT_PCMU         0
#define ULAW_SILENCE    0xFF
#define JB_SLOTS        16   /* power of two, > APP_JB_MAX_FRAMES */
#define TX_SB_BYTES     (RTP_FRAME_BYTES * 4)

_Static_assert(APP_JB_MAX_FRAMES < JB_SLOTS, "APP_JB_MAX_FRAMES must be < 16");
_Static_assert(APP_JB_PRIME_FRAMES >= 1 && APP_JB_PRIME_FRAMES <= APP_JB_MAX_FRAMES, "bad jitter buffer prime");

static int s_sock = -1;
static StreamBufferHandle_t s_tx_sb;

/* Session state, written by the SIP task, read by the RTP tasks. */
static _Atomic bool s_active;      /* accept rx, drain tx */
static _Atomic bool s_tx_enabled;  /* remote known, send */
static _Atomic bool s_rx_seen;     /* a PCMU packet arrived this session */
static portMUX_TYPE s_remote_mux = portMUX_INITIALIZER_UNLOCKED;
static struct sockaddr_in s_remote; /* where tx goes: SDP address, then the rx source */

/* TX header state, only touched by the sender task after session begin. */
static _Atomic uint32_t s_session_gen;
static uint16_t s_tx_seq;
static uint32_t s_tx_ts, s_tx_ssrc;
static bool s_tx_first;

/* ---------------------------------------------------------------------------
 * Jitter buffer. Pushed by the receiver task (core 0), pulled by the audio
 * task (core 1) in arbitrary chunk sizes. All fields under s_jb_mux.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t data[RTP_FRAME_BYTES];
    uint16_t seq;
    bool valid;
} jb_slot_t;

static portMUX_TYPE s_jb_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    jb_slot_t slot[JB_SLOTS];
    bool have_base;     /* first packet seen, play_seq is meaningful */
    bool playing;       /* primed, consuming */
    uint16_t play_seq;  /* next frame to play */
    uint16_t max_seq;   /* highest sequence received */
    size_t offset;      /* bytes of the play_seq frame already pulled */
    uint32_t ssrc;
    /* stats, reset every period */
    uint32_t rx, late, dup, lost, underruns, dropped;
} J;

static void jb_reset_locked(void)
{
    for (int i = 0; i < JB_SLOTS; i++) {
        J.slot[i].valid = false;
    }
    J.have_base = false;
    J.playing = false;
    J.offset = 0;
}

/* Frames buffered from play_seq up to the newest received (0 if none ahead). */
static int jb_depth_locked(void)
{
    if (!J.have_base) {
        return 0;
    }
    int d = (int16_t)(uint16_t)(J.max_seq - J.play_seq) + 1;
    return d < 0 ? 0 : d;
}

static void jb_rebase_locked(uint32_t ssrc, uint16_t seq)
{
    jb_reset_locked();
    J.have_base = true;
    J.ssrc = ssrc;
    J.play_seq = seq;
    J.max_seq = (uint16_t)(seq - 1); /* nothing received yet */
}

static void jb_push(uint32_t ssrc, uint16_t seq, const uint8_t *p, size_t n)
{
    taskENTER_CRITICAL(&s_jb_mux);
    if (!J.have_base || ssrc != J.ssrc) {
        jb_rebase_locked(ssrc, seq); /* first packet, or a new source */
    }
    int diff = (int16_t)(uint16_t)(seq - J.play_seq);
    if (diff >= JB_SLOTS * 4 || diff < -JB_SLOTS * 4) {
        jb_rebase_locked(ssrc, seq); /* big jump: sender restart or long outage */
        diff = 0;
    }
    if (diff < 0 || (diff == 0 && J.offset > 0)) {
        J.late++; /* its playout time has passed */
        taskEXIT_CRITICAL(&s_jb_mux);
        return;
    }
    if ((int16_t)(uint16_t)(seq - J.max_seq) > 0) {
        J.max_seq = seq;
    }
    /* Too deep: drop the oldest frames so latency stays bounded. */
    while (jb_depth_locked() > APP_JB_MAX_FRAMES) {
        J.slot[J.play_seq % JB_SLOTS].valid = false;
        J.play_seq++;
        J.offset = 0;
        J.dropped++;
    }
    if ((int16_t)(uint16_t)(seq - J.play_seq) >= 0) {
        jb_slot_t *s = &J.slot[seq % JB_SLOTS];
        if (s->valid && s->seq == seq) {
            J.dup++;
        } else {
            size_t c = n < RTP_FRAME_BYTES ? n : RTP_FRAME_BYTES;
            memcpy(s->data, p, c);
            memset(s->data + c, ULAW_SILENCE, RTP_FRAME_BYTES - c);
            s->seq = seq;
            s->valid = true;
            J.rx++;
        }
    }
    taskEXIT_CRITICAL(&s_jb_mux);
}

void rtp_audio_rx_pull(uint8_t *out, size_t n)
{
    taskENTER_CRITICAL(&s_jb_mux);
    while (n > 0) {
        if (!J.playing) {
            if (jb_depth_locked() >= APP_JB_PRIME_FRAMES) {
                J.playing = true;
            } else {
                memset(out, ULAW_SILENCE, n);
                break;
            }
        }
        if (J.offset == 0 && jb_depth_locked() == 0) {
            /* Nothing ahead at a frame boundary: re-prime. */
            J.playing = false;
            J.underruns++;
            continue;
        }
        jb_slot_t *s = &J.slot[J.play_seq % JB_SLOTS];
        bool ok = s->valid && s->seq == J.play_seq;
        size_t c = RTP_FRAME_BYTES - J.offset;
        if (c > n) {
            c = n;
        }
        if (ok) {
            memcpy(out, s->data + J.offset, c);
        } else {
            memset(out, ULAW_SILENCE, c); /* missing packet: conceal with silence */
        }
        out += c;
        n -= c;
        J.offset += c;
        if (J.offset >= RTP_FRAME_BYTES) {
            if (!ok) {
                J.lost++;
            }
            s->valid = false;
            J.play_seq++;
            J.offset = 0;
        }
    }
    taskEXIT_CRITICAL(&s_jb_mux);
}

/* ---------------------------------------------------------------------------
 * Audio TX side
 * ------------------------------------------------------------------------- */

static _Atomic uint32_t s_tx_overflow;

void rtp_audio_tx_push(const uint8_t *ulaw, size_t n)
{
    if (!s_tx_sb) {
        return;
    }
    if (xStreamBufferSend(s_tx_sb, ulaw, n, 0) < n) {
        atomic_fetch_add(&s_tx_overflow, 1);
    }
}

/* ---------------------------------------------------------------------------
 * Session control (SIP task)
 * ------------------------------------------------------------------------- */

static _Atomic uint32_t s_tx_pkts, s_tx_err;
static uint32_t s_rx_other;   /* non-PCMU or malformed, receiver task only */
static int64_t s_next_stats_us;

bool rtp_media_received(void)
{
    return atomic_load(&s_rx_seen);
}

uint16_t rtp_local_port(void)
{
    return APP_RTP_LOCAL_PORT;
}

void rtp_session_begin(void)
{
    atomic_store(&s_tx_enabled, false);
    atomic_store(&s_rx_seen, false);
    taskENTER_CRITICAL(&s_remote_mux);
    memset(&s_remote, 0, sizeof(s_remote));
    taskEXIT_CRITICAL(&s_remote_mux);
    taskENTER_CRITICAL(&s_jb_mux);
    jb_reset_locked();
    J.rx = J.late = J.dup = J.lost = J.underruns = J.dropped = 0;
    taskEXIT_CRITICAL(&s_jb_mux);
    atomic_store(&s_tx_pkts, 0);
    atomic_store(&s_tx_err, 0);
    atomic_store(&s_tx_overflow, 0);
    s_next_stats_us = esp_timer_get_time() + APP_STATS_PERIOD_MS * 1000LL;
    atomic_fetch_add(&s_session_gen, 1); /* sender picks new SSRC/seq/ts */
    atomic_store(&s_active, true);
}

void rtp_set_remote(const char *ip, uint16_t port)
{
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_aton(ip, &a.sin_addr) == 0 || port == 0) {
        ESP_LOGW(TAG, "bad remote media address %s:%u, not sending", ip, port);
        return;
    }
    bool changed;
    taskENTER_CRITICAL(&s_remote_mux);
    changed = s_remote.sin_addr.s_addr != a.sin_addr.s_addr || s_remote.sin_port != a.sin_port;
    if (changed) {
        s_remote = a;
    }
    taskEXIT_CRITICAL(&s_remote_mux);
    if (changed) {
        ESP_LOGI(TAG, "remote media %s:%u, sending from local port %u", ip, port, APP_RTP_LOCAL_PORT);
    }
    atomic_store(&s_tx_enabled, true);
}

static void log_stats(const char *what)
{
    uint32_t rx, late, dup, lost, und, drop;
    int depth;
    taskENTER_CRITICAL(&s_jb_mux);
    rx = J.rx; late = J.late; dup = J.dup; lost = J.lost; und = J.underruns; drop = J.dropped;
    depth = jb_depth_locked();
    J.rx = J.late = J.dup = J.lost = J.underruns = J.dropped = 0;
    taskEXIT_CRITICAL(&s_jb_mux);
    ESP_LOGI(TAG, "%s: tx %" PRIu32 " pkts (err %" PRIu32 ", audio overflow %" PRIu32 ") | rx %" PRIu32
             " pkts, lost %" PRIu32 ", late %" PRIu32 ", dup %" PRIu32 ", other %" PRIu32
             " | jb depth %d, underruns %" PRIu32 ", overflow drops %" PRIu32,
             what, atomic_exchange(&s_tx_pkts, 0), atomic_exchange(&s_tx_err, 0),
             atomic_exchange(&s_tx_overflow, 0), rx, lost, late, dup, s_rx_other, depth, und, drop);
    s_rx_other = 0;
}

void rtp_session_end(void)
{
    if (!atomic_exchange(&s_active, false)) {
        return;
    }
    atomic_store(&s_tx_enabled, false);
    log_stats("rtp final");
    taskENTER_CRITICAL(&s_jb_mux);
    jb_reset_locked();
    taskEXIT_CRITICAL(&s_jb_mux);
}

/* ---------------------------------------------------------------------------
 * Tasks
 * ------------------------------------------------------------------------- */

static void send_frame(const uint8_t *payload)
{
    struct sockaddr_in to;
    taskENTER_CRITICAL(&s_remote_mux);
    to = s_remote;
    taskEXIT_CRITICAL(&s_remote_mux);
    if (to.sin_port == 0) {
        return;
    }
    uint8_t pkt[12 + RTP_FRAME_BYTES];
    pkt[0] = 0x80;                                     /* V=2, no P/X/CC */
    pkt[1] = (s_tx_first ? 0x80 : 0x00) | PT_PCMU;     /* marker on the first packet */
    pkt[2] = s_tx_seq >> 8;
    pkt[3] = s_tx_seq & 0xff;
    pkt[4] = s_tx_ts >> 24;
    pkt[5] = (s_tx_ts >> 16) & 0xff;
    pkt[6] = (s_tx_ts >> 8) & 0xff;
    pkt[7] = s_tx_ts & 0xff;
    pkt[8] = s_tx_ssrc >> 24;
    pkt[9] = (s_tx_ssrc >> 16) & 0xff;
    pkt[10] = (s_tx_ssrc >> 8) & 0xff;
    pkt[11] = s_tx_ssrc & 0xff;
    memcpy(pkt + 12, payload, RTP_FRAME_BYTES);
    if (sendto(s_sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) == (int)sizeof(pkt)) {
        atomic_fetch_add(&s_tx_pkts, 1);
    } else {
        atomic_fetch_add(&s_tx_err, 1);
    }
    s_tx_first = false;
    s_tx_seq++;
    s_tx_ts += RTP_FRAME_BYTES;
}

static void tx_task(void *arg)
{
    (void)arg;
    uint8_t frame[RTP_FRAME_BYTES];
    size_t fill = 0;
    uint32_t gen = 0;
    while (1) {
        size_t n = xStreamBufferReceive(s_tx_sb, frame + fill, RTP_FRAME_BYTES - fill,
                                        pdMS_TO_TICKS(APP_RTP_TX_IDLE_MS));
        fill += n;
        bool send_ok = atomic_load(&s_active) && atomic_load(&s_tx_enabled);
        if (fill < RTP_FRAME_BYTES) {
            if (n > 0 || !send_ok) {
                if (!atomic_load(&s_active)) {
                    fill = 0;
                }
                continue;
            }
            /* Audio task supplied nothing for a while: send silence. */
            memset(frame + fill, ULAW_SILENCE, RTP_FRAME_BYTES - fill);
        }
        fill = 0;
        if (!send_ok) {
            continue; /* drop: no session or no remote yet */
        }
        uint32_t g = atomic_load(&s_session_gen);
        if (g != gen) {
            gen = g;
            s_tx_seq = (uint16_t)esp_random();
            s_tx_ts = esp_random();
            s_tx_ssrc = esp_random();
            s_tx_first = true;
            ESP_LOGI(TAG, "first RTP packet out, ssrc %08" PRIx32, s_tx_ssrc);
        }
        send_frame(frame);
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    static uint8_t buf[1500];
    uint32_t rx_gen = 0;
    while (1) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        bool active = atomic_load(&s_active);

        if (n > 0 && active) {
            size_t hdr = 12 + (size_t)(buf[0] & 0x0f) * 4;
            size_t len = (size_t)n;
            if (n < 12 || (buf[0] >> 6) != 2 || hdr > len) {
                s_rx_other++;
            } else {
                if (buf[0] & 0x10) { /* header extension */
                    if (hdr + 4 > len) {
                        hdr = len + 1;
                    } else {
                        hdr += 4 + (size_t)((buf[hdr + 2] << 8) | buf[hdr + 3]) * 4;
                    }
                }
                if ((buf[0] & 0x20) && hdr <= len && len > 0) { /* padding */
                    size_t pad = buf[len - 1];
                    len = pad <= len - hdr ? len - pad : 0;
                }
                uint8_t pt = buf[1] & 0x7f;
                if (hdr >= len || pt != PT_PCMU) {
                    s_rx_other++; /* telephone-event, comfort noise, malformed */
                } else {
                    uint16_t seq = (uint16_t)((buf[2] << 8) | buf[3]);
                    uint32_t ssrc = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                                    ((uint32_t)buf[10] << 8) | buf[11];
                    /* Symmetric RTP: always send back to where media comes from. */
                    bool latch = false;
                    taskENTER_CRITICAL(&s_remote_mux);
                    if (s_remote.sin_addr.s_addr != from.sin_addr.s_addr || s_remote.sin_port != from.sin_port) {
                        s_remote = from;
                        latch = true;
                    }
                    taskEXIT_CRITICAL(&s_remote_mux);
                    if (latch) {
                        char ip[16];
                        inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
                        ESP_LOGW(TAG, "media arrives from %s:%u (not the SDP address), sending there",
                                 ip, ntohs(from.sin_port));
                        atomic_store(&s_tx_enabled, true);
                    }
                    uint32_t g = atomic_load(&s_session_gen);
                    if (g != rx_gen) {
                        rx_gen = g;
                        ESP_LOGI(TAG, "first RTP packet in, ssrc %08" PRIx32 ", %u byte payload",
                                 ssrc, (unsigned)(len - hdr));
                    }
                    jb_push(ssrc, seq, buf + hdr, len - hdr);
                    atomic_store(&s_rx_seen, true);
                }
            }
        }

        if (active) {
            int64_t now = esp_timer_get_time();
            if (now >= s_next_stats_us) {
                s_next_stats_us = now + APP_STATS_PERIOD_MS * 1000LL;
                log_stats("rtp 5s");
            }
        }
    }
}

bool rtp_start(void)
{
    s_tx_sb = xStreamBufferCreate(TX_SB_BYTES, RTP_FRAME_BYTES);
    if (!s_tx_sb) {
        ESP_LOGE(TAG, "stream buffer alloc failed");
        return false;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        return false;
    }
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(APP_RTP_LOCAL_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        ESP_LOGE(TAG, "bind to port %d failed: errno %d", APP_RTP_LOCAL_PORT, errno);
        close(s_sock);
        s_sock = -1;
        return false;
    }
    /* Receive timeout so the receiver can print stats with no traffic. */
    struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    xTaskCreatePinnedToCore(tx_task, "rtp_tx", 4096, NULL, 7, NULL, 0);
    xTaskCreatePinnedToCore(rx_task, "rtp_rx", 4096, NULL, 7, NULL, 0);
    ESP_LOGI(TAG, "media socket on UDP port %d", APP_RTP_LOCAL_PORT);
    return true;
}
