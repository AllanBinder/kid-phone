#include "secret.h"

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"

#include "app_config.h"
#include "config_store.h"
#include "mime.h"
#include "modes.h"
#include "prompts.h"
#include "resampler.h"
#include "schedule.h"
#include "sip.h"
#include "smtp.h"
#include "tz_posix.h"
#include "usb_audio.h"
#include "wav.h"
#include "wifi.h"

static const char *TAG = "secret";

/* Steps of one secret (button task; read by the audio and light tasks). */
typedef enum {
    FLOW_IDLE,
    FLOW_PROMPT,   /* "Tell your secret after the beep!" */
    FLOW_BEEP,     /* the beep, then a short gap */
    FLOW_REC,      /* recording */
    FLOW_STOPPING, /* stop asked, waiting for the audio task to let go of the buffer */
    FLOW_END_BEEP, /* length limit reached: "time's up" beep */
} flow_t;

static _Atomic int s_flow = FLOW_IDLE;
static _Atomic int s_flow_slot = -1;
static int64_t s_flow_t0; /* when the current step began (button task) */

/* Recorder hand-off: the button task owns the buffer while REC_OFF (and
 * once REC_DONE), the audio task while REC_ON and REC_STOP. */
enum { REC_OFF, REC_ON, REC_STOP, REC_DONE };
static _Atomic int s_rec = REC_OFF;
static uint8_t *s_buf;    /* WAV header + samples, PSRAM */
static size_t s_cap;      /* samples that fit */
static size_t s_n;        /* samples recorded */
static uint32_t s_in_rate;
static resampler_t s_dec;
static bool s_full;       /* stopped at the length limit */
static bool s_bad;        /* the mic went away or changed rate */

/* The one secret waiting to be sent, and the status for the page. All under
 * s_lock. */
typedef struct {
    bool used;
    bool sending;
    bool boosted;   /* wav_normalize done */
    uint8_t *wav;   /* PSRAM, freed once sent or given up */
    size_t len;
    size_t samples;
    int slot;
    char to[64];
    char name[32];
    int64_t utc;    /* when it was recorded, 0 = clock not synced */
    int tries;
    int64_t next_ms;
} queued_t;
static queued_t s_q;

typedef struct {
    uint32_t seq;
    char state[12];
    char msg[200];
    int slot;
    uint32_t secs;
    int64_t at_ms;
    int64_t utc;
} status_t;
static status_t s_last = {.state = "none", .slot = -1};
static status_t s_test = {.state = "none", .slot = -1};
static bool s_test_pending;

/* Start times of the secrets queued in the last hour (the cap). */
static int64_t s_stamps[APP_SECRET_CAP_HI];
static int s_nstamps;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* Caller holds the lock. */
static void set_status(status_t *st, const char *state, const char *msg)
{
    st->seq++;
    strlcpy(st->state, state, sizeof(st->state));
    strlcpy(st->msg, msg ? msg : "", sizeof(st->msg));
    st->at_ms = now_ms();
}

/* Caller holds the lock. Drops stamps older than an hour, returns the rest. */
static int recent_locked(void)
{
    const int64_t cut = now_ms() - 3600 * 1000LL;
    int k = 0;
    for (int i = 0; i < s_nstamps; i++) {
        if (s_stamps[i] > cut) {
            s_stamps[k++] = s_stamps[i];
        }
    }
    s_nstamps = k;
    return k;
}

static void stamp_locked(void)
{
    recent_locked();
    if (s_nstamps == APP_SECRET_CAP_HI) {
        memmove(s_stamps, s_stamps + 1, sizeof(s_stamps[0]) * (APP_SECRET_CAP_HI - 1));
        s_nstamps--;
    }
    s_stamps[s_nstamps++] = now_ms();
}

bool secret_active(void)
{
    return atomic_load(&s_flow) != FLOW_IDLE;
}

int secret_slot(void)
{
    return secret_active() ? atomic_load(&s_flow_slot) : -1;
}

int secret_audio_mode(void)
{
    switch (atomic_load(&s_flow)) {
    case FLOW_BEEP:
    case FLOW_END_BEEP:
        return MODE_BEEP;
    case FLOW_REC:
    case FLOW_STOPPING:
        return MODE_SECRET;
    default:
        return -1;
    }
}

bool secret_slot_ready(int slot, bool asleep)
{
    cfg_secret_opts_t o;
    config_secret_opts(&o);
    if (!o.configured || (asleep && !o.bed_ok) || !wifi_is_connected() ||
        config_slot_secret(slot, NULL, 0) == CFG_SECRET_OFF) {
        return false;
    }
    lock();
    const bool ok = !s_q.used && recent_locked() < o.cap_hr;
    unlock();
    return ok;
}

/* ---------------------------------------------------------------------------
 * Recording (button task side)
 * ------------------------------------------------------------------------- */

static void free_recording(void)
{
    resampler_deinit(&s_dec);
    heap_caps_free(s_buf);
    s_buf = NULL;
}

secret_begin_t secret_begin(int slot)
{
    if (secret_active()) {
        return SECRET_BUSY;
    }
    cfg_secret_opts_t o;
    config_secret_opts(&o);
    char to[64];
    const uint8_t mode = config_slot_secret(slot, to, sizeof(to));
    if (!o.configured || mode == CFG_SECRET_OFF || !to[0]) {
        return SECRET_NOT_SET_UP;
    }
    if (!wifi_is_connected()) {
        return SECRET_NO_WIFI;
    }
    lock();
    const bool waiting = s_q.used;
    const int recent = recent_locked();
    unlock();
    if (waiting) {
        return SECRET_BUSY;
    }
    if (recent >= o.cap_hr) {
        return SECRET_CAP;
    }
    uint32_t rate = 0;
    bool spk = false;
    if (usb_audio_lock(pdMS_TO_TICKS(50))) {
        const ua_state_t *st = usb_audio_state();
        rate = st->mic.active ? st->mic.rate : 0;
        spk = st->spk.active;
        usb_audio_unlock();
    }
    if (!spk || !rate || rate % APP_SECRET_RATE != 0) {
        ESP_LOGW(TAG, "no handset (or mic at %" PRIu32 " Hz, not a multiple of %d)", rate, APP_SECRET_RATE);
        return SECRET_NO_HANDSET;
    }
    s_cap = (size_t)o.max_s * APP_SECRET_RATE;
    s_buf = heap_caps_malloc(WAV_HEADER_BYTES + s_cap * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf || !resampler_decim_init(&s_dec, (int)(rate / APP_SECRET_RATE), (float)rate, APP_SECRET_CUTOFF_HZ)) {
        ESP_LOGE(TAG, "no memory for a %u s recording (%u bytes of PSRAM)", o.max_s,
                 (unsigned)(WAV_HEADER_BYTES + s_cap * 2));
        free_recording();
        return SECRET_NO_MEMORY;
    }
    s_n = 0;
    s_full = s_bad = false;
    s_in_rate = rate;
    atomic_store(&s_rec, REC_OFF);
    atomic_store(&s_flow_slot, slot);
    atomic_store(&s_flow, FLOW_PROMPT);
    s_flow_t0 = now_ms();
    lock();
    s_last.slot = slot;
    s_last.secs = 0;
    s_last.utc = 0;
    set_status(&s_last, "recording", NULL);
    unlock();
    ESP_LOGI(TAG, "secret for button %d: up to %u s, mic %" PRIu32 " Hz, %u KB buffer", slot + 1, o.max_s, rate,
             (unsigned)((WAV_HEADER_BYTES + s_cap * 2) / 1024));
    prompt_play(PROMPT_SECRET_START);
    return SECRET_STARTED;
}

void secret_press(int slot)
{
    if (atomic_load(&s_flow) != FLOW_REC || slot != atomic_load(&s_flow_slot)) {
        return;
    }
    int on = REC_ON;
    atomic_compare_exchange_strong(&s_rec, &on, REC_STOP);
    atomic_store(&s_flow, FLOW_STOPPING);
    s_flow_t0 = now_ms();
    ESP_LOGI(TAG, "button %d pressed: stop recording", slot + 1);
}

/* The recording is over: throw it away or queue it. */
static void finish(void)
{
    const int slot = atomic_load(&s_flow_slot);
    const size_t n = s_n;
    const uint32_t ms = (uint32_t)(n * 1000 / APP_SECRET_RATE);
    resampler_deinit(&s_dec);
    atomic_store(&s_rec, REC_OFF);
    if (s_bad || ms < APP_SECRET_MIN_MS) {
        ESP_LOGI(TAG, "secret thrown away (%s, %" PRIu32 " ms)", s_bad ? "handset gone" : "too short", ms);
        free_recording();
        lock();
        s_last.secs = ms / 1000;
        set_status(&s_last, "discarded", s_bad ? "The handset stopped working during the recording."
                                               : "Shorter than 2 seconds, so it was not sent.");
        unlock();
        return;
    }
    wav_header(s_buf, APP_SECRET_RATE, n);
    cfg_contact_t *c = malloc(sizeof(cfg_contact_t));
    lock();
    memset(&s_q, 0, sizeof(s_q));
    s_q.used = true;
    s_q.wav = s_buf;
    s_q.len = WAV_HEADER_BYTES + n * 2;
    s_q.samples = n;
    s_q.slot = slot;
    config_slot_secret(slot, s_q.to, sizeof(s_q.to));
    if (c) {
        config_get_contact(slot, c);
        strlcpy(s_q.name, c->name, sizeof(s_q.name));
    }
    s_q.utc = schedule_time_synced() ? (int64_t)time(NULL) : 0;
    stamp_locked();
    s_last.secs = ms / 1000;
    s_last.utc = s_q.utc;
    set_status(&s_last, "queued", NULL);
    unlock();
    free(c);
    s_buf = NULL; /* the queue owns it now */
    ESP_LOGI(TAG, "secret for button %d queued: %.1f s, %u bytes", slot + 1, ms / 1000.0, (unsigned)s_q.len);
    prompt_play(PROMPT_SECRET_SENT);
    xTaskNotifyGive(s_task);
}

bool secret_poll(void)
{
    const int64_t t = now_ms() - s_flow_t0;
    switch (atomic_load(&s_flow)) {
    case FLOW_PROMPT:
        if (!prompt_active()) {
            atomic_store(&s_flow, FLOW_BEEP);
            s_flow_t0 = now_ms();
        }
        return false;
    case FLOW_BEEP:
        if (t >= APP_SECRET_BEEP_MS + APP_SECRET_BEEP_GAP_MS) {
            atomic_store(&s_flow, FLOW_REC);
            s_flow_t0 = now_ms();
            atomic_store(&s_rec, REC_ON);
            ESP_LOGI(TAG, "recording");
        }
        return false;
    case FLOW_REC:
        if (atomic_load(&s_rec) == REC_DONE) {
            break;
        }
        /* The audio task stopped feeding (handset unplugged): stop anyway. */
        if (t > (int64_t)s_cap * 1000 / APP_SECRET_RATE + 3000) {
            ESP_LOGW(TAG, "recording ran past its length: stopping");
            int on = REC_ON;
            atomic_compare_exchange_strong(&s_rec, &on, REC_STOP);
            atomic_store(&s_flow, FLOW_STOPPING);
            s_flow_t0 = now_ms();
        }
        return false;
    case FLOW_STOPPING:
        if (atomic_load(&s_rec) == REC_DONE) {
            break;
        }
        if (t > 1000) {
            /* The audio task never answered; it is not touching the buffer
             * either (it only does between the stop check and the write). */
            ESP_LOGW(TAG, "audio task did not finish the stop, taking the buffer back");
            atomic_store(&s_rec, REC_DONE);
            break;
        }
        return false;
    case FLOW_END_BEEP:
        if (t >= APP_SECRET_BEEP_MS + APP_SECRET_BEEP_GAP_MS) {
            finish();
            atomic_store(&s_flow, FLOW_IDLE);
            return true;
        }
        return false;
    default:
        return false;
    }
    /* REC_DONE: the audio task has let go of the buffer. */
    if (s_full) {
        ESP_LOGI(TAG, "length limit reached");
        atomic_store(&s_flow, FLOW_END_BEEP);
        s_flow_t0 = now_ms();
        return false;
    }
    finish();
    atomic_store(&s_flow, FLOW_IDLE);
    return true;
}

/* ---------------------------------------------------------------------------
 * Recording (audio task side)
 * ------------------------------------------------------------------------- */

void secret_audio_feed(const int16_t *mono, size_t n, uint32_t rate)
{
    static int16_t tmp[480];
    if (atomic_load(&s_rec) != REC_ON) {
        return;
    }
    if (rate != s_in_rate) {
        s_bad = true;
        atomic_store(&s_rec, REC_DONE);
        return;
    }
    int16_t *pcm = (int16_t *)(s_buf + WAV_HEADER_BYTES);
    size_t pos = 0;
    while (pos < n) {
        const size_t take = n - pos < sizeof(tmp) / sizeof(tmp[0]) ? n - pos : sizeof(tmp) / sizeof(tmp[0]);
        size_t k = resampler_decim_process(&s_dec, mono + pos, take, tmp);
        pos += take;
        if (k > s_cap - s_n) {
            k = s_cap - s_n;
        }
        memcpy(pcm + s_n, tmp, k * sizeof(int16_t));
        s_n += k;
        if (s_n == s_cap) {
            s_full = true;
            atomic_store(&s_rec, REC_DONE);
            return;
        }
    }
}

void secret_audio_tick(bool have_mic)
{
    int r = atomic_load(&s_rec);
    if (r == REC_STOP) {
        atomic_store(&s_rec, REC_DONE);
    } else if (r == REC_ON && !have_mic) {
        s_bad = true;
        atomic_store(&s_rec, REC_DONE);
    }
}

/* ---------------------------------------------------------------------------
 * Sending
 * ------------------------------------------------------------------------- */

/* Local time in the home time zone (bedtime card). Returns false if the
 * clock or the zone is not known; then tm is UTC and off 0. */
static bool home_time(int64_t utc, struct tm *tm, int32_t *off)
{
    cfg_bedtime_t b;
    config_get_bedtime(&b);
    tz_posix_t tz;
    *off = 0;
    bool ok = b.tz[0] && tz_posix_parse(b.tz, &tz);
    if (ok) {
        *off = tz_posix_utc_offset(&tz, utc);
    }
    time_t lt = (time_t)(utc + *off);
    gmtime_r(&lt, tm);
    return ok;
}

/* "7:42 pm" */
static void clock_12h(const struct tm *tm, char *out, size_t len)
{
    const int h = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
    snprintf(out, len, "%d:%02d %s", h, tm->tm_min, tm->tm_hour < 12 ? "am" : "pm");
}

typedef struct {
    char subject[160];
    char text[640];
    char date[48];
    char msg_id[64];
    char boundary[48];
    char file[64];
} mail_parts_t;

static void make_ids(mail_parts_t *p)
{
    const uint32_t a = esp_random(), b = esp_random();
    snprintf(p->msg_id, sizeof(p->msg_id), "<kidphone.%08" PRIx32 "%08" PRIx32 "@%s.local>", a, b, APP_HOSTNAME);
    snprintf(p->boundary, sizeof(p->boundary), "kidphone-%08" PRIx32 "%08" PRIx32, b, a);
}

/* Date header from the home zone (UTC if it is not set). */
static void date_header(int64_t utc, char *out, size_t len)
{
    struct tm tm;
    int32_t off;
    home_time(utc, &tm, &off);
    static const char *wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int32_t a = off < 0 ? -off : off;
    snprintf(out, len, "%s, %d %s %d %02d:%02d:%02d %c%02d%02d", wd[tm.tm_wday], tm.tm_mday, mo[tm.tm_mon],
             tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec, off < 0 ? '-' : '+', (int)(a / 3600),
             (int)(a / 60 % 60));
}

static void compose_secret(const queued_t *q, const cfg_secret_t *acct, mail_parts_t *p)
{
    make_ids(p);
    const char *kid = acct->kid_name[0] ? acct->kid_name : NULL;
    char when[24] = "", day[48] = "";
    strlcpy(p->file, "secret.wav", sizeof(p->file));
    p->date[0] = '\0';
    if (q->utc) {
        struct tm tm;
        int32_t off;
        const bool local = home_time(q->utc, &tm, &off);
        static const char *wday[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        static const char *mon[] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                    "September", "October", "November", "December"};
        clock_12h(&tm, when, sizeof(when));
        if (!local) {
            strlcat(when, " UTC", sizeof(when));
        }
        snprintf(day, sizeof(day), "%s %d %s %d", wday[tm.tm_wday], tm.tm_mday, mon[tm.tm_mon], tm.tm_year + 1900);
        snprintf(p->file, sizeof(p->file), "secret-%04d%02d%02d-%02d%02d.wav", tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min);
        date_header(q->utc, p->date, sizeof(p->date));
    }
    /* U+1F92B shushing face. */
    snprintf(p->subject, sizeof(p->subject), "\xF0\x9F\xA4\xAB A secret from %s%s%s", kid ? kid : "the Kid Phone",
             when[0] ? ", " : "", when);
    const unsigned secs = (unsigned)((q->samples + APP_SECRET_RATE / 2) / APP_SECRET_RATE);
    char who[96];
    if (q->name[0]) {
        snprintf(who, sizeof(who), " for %s", q->name);
    } else {
        who[0] = '\0';
    }
    char at[96] = "";
    if (day[0]) {
        snprintf(at, sizeof(at), "Recorded %s at %s.\n", day, when);
    }
    snprintf(p->text, sizeof(p->text),
             "%s recorded a secret%s on the Kid Phone.\n"
             "%s"
             "Length: %u second%s. The recording is attached (%s).\n"
             "\n"
             "The phone sent this itself, from your own email account. It cannot read replies.\n",
             kid ? kid : "Someone", who, at, secs, secs == 1 ? "" : "s", p->file);
}

/* What the page says for a failed send. */
static void friendly(smtp_err_t e, const char *detail, const cfg_secret_t *a, char *out, size_t len)
{
    switch (e) {
    case SMTP_ERR_DNS:
        snprintf(out, len, "Can't find the mail server %.64s. Check the server name.", a->host);
        break;
    case SMTP_ERR_CONNECT:
        snprintf(out, len, "Can't reach the mail server %.64s on port %u. Check the server and port.", a->host,
                 a->port);
        break;
    case SMTP_ERR_TLS:
        snprintf(out, len, "The secure connection failed (%.100s). Check the server and port.", detail);
        break;
    case SMTP_ERR_NO_TLS:
        snprintf(out, len, "The server does not offer STARTTLS on port %u. Try port 465 with SSL/TLS.", a->port);
        break;
    case SMTP_ERR_AUTH:
        snprintf(out, len, "Login rejected. Check the email address and the app password (%.100s).", detail);
        break;
    case SMTP_ERR_NO_AUTH:
        snprintf(out, len, "The server does not accept a password login.");
        break;
    case SMTP_ERR_REJECTED:
        snprintf(out, len, "The mail server refused it (%.120s).", detail);
        break;
    case SMTP_ERR_TEMP:
        snprintf(out, len, "The mail server said to try later (%.120s).", detail);
        break;
    case SMTP_ERR_IO:
        snprintf(out, len, "The connection to the mail server dropped (%.100s).", detail);
        break;
    default:
        snprintf(out, len, "Out of memory.");
        break;
    }
}

/* One send attempt with the saved account. */
static smtp_err_t send_now(const cfg_secret_t *acct, const char *to, const mail_parts_t *p, const uint8_t *att,
                           size_t att_len, char *detail, size_t dl)
{
    char ip[16] = "", helo[24];
    wifi_get_ip(ip, sizeof(ip));
    snprintf(helo, sizeof(helo), "[%s]", ip[0] ? ip : "127.0.0.1");
    const smtp_account_t a = {
        .host = acct->host,
        .port = acct->port,
        .security = acct->security ? SMTP_STARTTLS : SMTP_TLS,
        .user = acct->user,
        .pass = acct->pass,
        .helo = helo,
    };
    const mime_msg_t m = {
        .from_name = acct->from_name[0] ? acct->from_name : APP_SECRET_FROM_DEFAULT,
        .from_addr = acct->user,
        .to_addr = to,
        .subject = p->subject,
        .date = p->date[0] ? p->date : NULL,
        .message_id = p->msg_id,
        .boundary = p->boundary,
        .text = p->text,
        .att = att,
        .att_len = att_len,
        .att_name = p->file,
        .att_type = "audio/wav",
    };
    const int64_t t0 = now_ms();
    smtp_err_t e = smtp_send(&a, &m, detail, dl);
    ESP_LOGI(TAG, "send %s in %lld ms (%u byte attachment), stack left %u", smtp_err_name(e),
             (long long)(now_ms() - t0), (unsigned)att_len, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return e;
}

static void run_test(cfg_secret_t *acct, mail_parts_t *p)
{
    make_ids(p);
    strlcpy(p->subject, "Kid Phone test email", sizeof(p->subject));
    p->date[0] = '\0';
    if (schedule_time_synced()) {
        date_header((int64_t)time(NULL), p->date, sizeof(p->date));
    }
    snprintf(p->text, sizeof(p->text),
             "This is a test from your Kid Phone.\n"
             "\n"
             "Secret messages will arrive like this, from this account, with the recording attached.\n");
    char detail[200], msg[200];
    smtp_err_t e = send_now(acct, acct->user, p, NULL, 0, detail, sizeof(detail));
    if (e != SMTP_OK) {
        friendly(e, detail, acct, msg, sizeof(msg));
    }
    lock();
    s_test_pending = false;
    set_status(&s_test, e == SMTP_OK ? "sent" : "failed", e == SMTP_OK ? NULL : msg);
    unlock();
}

static int64_t backoff_ms(int tries)
{
    int64_t ms = APP_SECRET_RETRY_MIN_MS;
    for (int i = 1; i < tries && ms < APP_SECRET_RETRY_MAX_MS; i++) {
        ms *= 2;
    }
    return ms < APP_SECRET_RETRY_MAX_MS ? ms : APP_SECRET_RETRY_MAX_MS;
}

static void drop_queued_locked(void)
{
    heap_caps_free(s_q.wav);
    memset(&s_q, 0, sizeof(s_q));
}

static void run_queued(cfg_secret_t *acct, mail_parts_t *p)
{
    lock();
    queued_t q = s_q; /* the buffer stays put: only this task frees it */
    s_q.sending = true;
    set_status(&s_last, "sending", NULL);
    unlock();
    if (!q.boosted) {
        const float g = wav_normalize((int16_t *)(q.wav + WAV_HEADER_BYTES), q.samples, APP_SECRET_MAX_GAIN);
        ESP_LOGI(TAG, "level boost x%.2f", g);
        lock();
        s_q.boosted = true;
        unlock();
    }
    compose_secret(&q, acct, p);
    char detail[200], msg[200];
    smtp_err_t e = send_now(acct, q.to, p, q.wav, q.len, detail, sizeof(detail));
    lock();
    s_q.sending = false;
    if (e == SMTP_OK) {
        drop_queued_locked();
        set_status(&s_last, "sent", NULL);
        ESP_LOGI(TAG, "secret sent");
    } else {
        friendly(e, detail, acct, msg, sizeof(msg));
        s_q.tries++;
        if (smtp_err_permanent(e) || s_q.tries >= APP_SECRET_TRIES) {
            drop_queued_locked();
            set_status(&s_last, "failed", msg);
            ESP_LOGW(TAG, "secret given up after %d tr%s", q.tries + 1, q.tries ? "ies" : "y");
        } else {
            const int64_t wait = backoff_ms(s_q.tries);
            s_q.next_ms = now_ms() + wait;
            char m2[sizeof(msg) + 48];
            snprintf(m2, sizeof(m2), "%s Trying again in %lld min.", msg, (long long)((wait + 59999) / 60000));
            set_status(&s_last, "retrying", m2);
        }
    }
    unlock();
}

static void send_task(void *arg)
{
    (void)arg;
    /* Settings and message parts on the heap: this stack is for TLS. */
    cfg_secret_t *acct = malloc(sizeof(cfg_secret_t));
    mail_parts_t *parts = malloc(sizeof(mail_parts_t));
    assert(acct && parts);
    uint32_t gen = config_generation();
    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        const uint32_t g = config_generation();
        lock();
        if (g != gen && s_q.used) {
            s_q.next_ms = 0; /* settings changed: maybe fixed, try now */
        }
        const bool test = s_test_pending;
        const bool due = s_q.used && now_ms() >= s_q.next_ms;
        unlock();
        gen = g;
        /* Not during a call: the upload would compete with the call audio. */
        if ((!test && !due) || !wifi_is_connected() || sip_call_state() != CALL_IDLE) {
            continue;
        }
        config_get_secret(acct);
        if (!acct->host[0] || !acct->user[0] || !acct->pass[0]) {
            lock();
            if (test) {
                s_test_pending = false;
                set_status(&s_test, "failed", "Fill in the email settings and press Save first.");
            }
            if (due) {
                s_q.next_ms = now_ms() + 60000; /* wait for the settings, no try used */
            }
            unlock();
        } else if (test) {
            run_test(acct, parts);
        } else {
            run_queued(acct, parts);
        }
        mbedtls_platform_zeroize(acct, sizeof(*acct));
    }
}

void secret_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    assert(s_lock);
    xTaskCreatePinnedToCore(send_task, "secret_send", APP_SECRET_STACK, NULL, 2, &s_task, 0);
}

bool secret_test(char *err, size_t err_len)
{
    cfg_secret_opts_t o;
    config_secret_opts(&o);
    if (!o.configured) {
        snprintf(err, err_len, "Fill in the email settings and press Save first.");
        return false;
    }
    if (!wifi_is_connected()) {
        snprintf(err, err_len, "The phone is not on WiFi.");
        return false;
    }
    lock();
    const bool busy = s_test_pending || s_q.sending;
    if (!busy) {
        s_test_pending = true;
        set_status(&s_test, "sending", NULL);
    }
    unlock();
    if (busy) {
        snprintf(err, err_len, "The phone is already sending an email. Try again in a minute.");
        return false;
    }
    xTaskNotifyGive(s_task);
    return true;
}

static void status_obj(cJSON *o, const status_t *st)
{
    cJSON_AddNumberToObject(o, "seq", st->seq);
    cJSON_AddStringToObject(o, "state", st->state);
    cJSON_AddStringToObject(o, "message", st->msg);
    if (st->seq) {
        cJSON_AddNumberToObject(o, "ago_s", (double)((now_ms() - st->at_ms) / 1000));
    }
}

void secret_status_json(cJSON *o)
{
    lock();
    status_t last = s_last, test = s_test;
    const int recent = recent_locked();
    const bool waiting = s_q.used;
    unlock();
    cJSON *l = cJSON_AddObjectToObject(o, "last");
    status_obj(l, &last);
    cJSON_AddNumberToObject(l, "slot", last.slot);
    cJSON_AddNumberToObject(l, "seconds", last.secs);
    if (last.utc) {
        cJSON_AddNumberToObject(l, "utc", (double)last.utc);
    }
    status_obj(cJSON_AddObjectToObject(o, "test"), &test);
    cJSON_AddNumberToObject(o, "last_hour", recent);
    cJSON_AddBoolToObject(o, "waiting", waiting);
    cJSON_AddBoolToObject(o, "recording", secret_active());
}
