#include "config_store.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "pins.h"
#include "prompts.h"

/* Optional developer override (gitignored). The build works without it. */
#if __has_include("secrets.h")
#include "secrets.h"
#endif

static const char *TAG = "config";
static const char *NS = "kidphone";

static app_cfg_t s_cfg;
static SemaphoreHandle_t s_lock;
static uint32_t s_gen;

/* Saved WiFi networks, oldest first, packed from index 0. */
static cfg_wifi_t s_wifi[CFG_MAX_WIFI];
static int s_nwifi;

/* Stored schedule (NVS blob "c<i>_sch"). Fixed layout, versioned so a later
 * firmware can extend it; a blob with an unknown version or size is ignored
 * (the contact is then always available). */
#define SCHED_BLOB_VERSION 1
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t enabled;
    uint8_t nwin;
    char tz[48];
    char tz_name[40];
    struct __attribute__((packed)) {
        uint8_t days;
        uint16_t start_min;
        uint16_t end_min;
    } win[CFG_MAX_WINDOWS];
} sched_blob_t;
_Static_assert(sizeof(sched_blob_t) == 111, "schedule blob layout changed: bump SCHED_BLOB_VERSION");

/* Stored pins (NVS blob "pins"), same rules: a blob with an unknown version
 * or size, or one that fails pins_validate, is ignored and the phone uses
 * the 0.4.x wiring (pins_defaults). Up to 32 slots fit this version, so a
 * later firmware with a larger CFG_MAX_CONTACTS still reads it. */
#define PINS_BLOB_VERSION 1
#define PINS_BLOB_SLOTS   32
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t count;
    int8_t button[PINS_BLOB_SLOTS];
    int8_t lamp[PINS_BLOB_SLOTS];
} pins_blob_t;
_Static_assert(sizeof(pins_blob_t) == 66, "pins blob layout changed: bump PINS_BLOB_VERSION");
_Static_assert(CFG_MAX_CONTACTS <= PINS_BLOB_SLOTS, "pins blob too small for CFG_MAX_CONTACTS");

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) {
        out[0] = '\0';
    }
}

static void sched_from_blob(const sched_blob_t *b, cfg_schedule_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = b->enabled != 0;
    memcpy(out->tz, b->tz, sizeof(out->tz));
    out->tz[sizeof(out->tz) - 1] = '\0';
    memcpy(out->tz_name, b->tz_name, sizeof(out->tz_name));
    out->tz_name[sizeof(out->tz_name) - 1] = '\0';
    out->nwin = b->nwin > CFG_MAX_WINDOWS ? CFG_MAX_WINDOWS : b->nwin;
    for (int i = 0; i < out->nwin; i++) {
        out->win[i].days = b->win[i].days & 0x7f;
        out->win[i].start_min = b->win[i].start_min % 1440;
        out->win[i].end_min = b->win[i].end_min % 1440;
    }
}

static void sched_to_blob(const cfg_schedule_t *in, sched_blob_t *b)
{
    memset(b, 0, sizeof(*b));
    b->version = SCHED_BLOB_VERSION;
    b->enabled = in->enabled;
    strlcpy(b->tz, in->tz, sizeof(b->tz));
    strlcpy(b->tz_name, in->tz_name, sizeof(b->tz_name));
    b->nwin = in->nwin > CFG_MAX_WINDOWS ? CFG_MAX_WINDOWS : in->nwin;
    for (int i = 0; i < b->nwin; i++) {
        b->win[i].days = in->win[i].days;
        b->win[i].start_min = in->win[i].start_min;
        b->win[i].end_min = in->win[i].end_min;
    }
}

static void load_sched(nvs_handle_t h, int idx, cfg_schedule_t *out)
{
    char k[12];
    snprintf(k, sizeof(k), "c%d_sch", idx);
    sched_blob_t b;
    size_t n = sizeof(b);
    memset(out, 0, sizeof(*out));
    if (nvs_get_blob(h, k, &b, &n) == ESP_OK && n == sizeof(b) && b.version == SCHED_BLOB_VERSION) {
        sched_from_blob(&b, out);
    }
}

static void load_pins(nvs_handle_t h, cfg_pins_t *out)
{
    pins_defaults(out);
    pins_blob_t b;
    size_t n = sizeof(b);
    if (nvs_get_blob(h, "pins", &b, &n) != ESP_OK) {
        return; /* never saved: the 0.4.x wiring */
    }
    cfg_pins_t p;
    memset(&p, 0, sizeof(p));
    if (n != sizeof(b) || b.version != PINS_BLOB_VERSION || b.count > CFG_MAX_CONTACTS) {
        ESP_LOGW(TAG, "saved pins unreadable (version %u, %u bytes): using the default wiring", b.version,
                 (unsigned)n);
        return;
    }
    p.count = b.count;
    memcpy(p.button, b.button, sizeof(p.button));
    memcpy(p.lamp, b.lamp, sizeof(p.lamp));
    char why[96];
    if (pins_validate(&p, why, sizeof(why))) {
        ESP_LOGW(TAG, "saved pins not usable (%s): using the default wiring", why);
        return;
    }
    *out = p;
}

/* Erase one key, missing is fine. */
static esp_err_t erase_key(nvs_handle_t h, const char *key)
{
    esp_err_t err = nvs_erase_key(h, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

/*
 * Write names, numbers and schedules of all slots (caller holds the lock).
 * A key whose value is empty (no name, no number, schedule off with no
 * times) is erased instead of written: it loads back the same (missing =
 * empty), also in firmware before 0.5.0, and CFG_MAX_CONTACTS slots of
 * empty keys would fill the small NVS partition.
 */
static esp_err_t save_contacts_locked(nvs_handle_t h)
{
    esp_err_t err = ESP_OK;
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        const cfg_contact_t *c = &s_cfg.contacts[i];
        char k[12];
        snprintf(k, sizeof(k), "c%d_name", i);
        err |= c->name[0] ? nvs_set_str(h, k, c->name) : erase_key(h, k);
        snprintf(k, sizeof(k), "c%d_num", i);
        err |= c->number[0] ? nvs_set_str(h, k, c->number) : erase_key(h, k);
        snprintf(k, sizeof(k), "c%d_sch", i);
        if (c->sched.enabled || c->sched.nwin || c->sched.tz[0]) {
            sched_blob_t b;
            sched_to_blob(&c->sched, &b);
            err |= nvs_set_blob(h, k, &b, sizeof(b));
        } else {
            err |= erase_key(h, k);
        }
        /* Secrets: mode and email only while the mode is not off. */
        const bool sec = c->secret != CFG_SECRET_OFF && c->email[0];
        snprintf(k, sizeof(k), "c%d_sec", i);
        err |= sec ? nvs_set_u8(h, k, c->secret) : erase_key(h, k);
        snprintf(k, sizeof(k), "c%d_mail", i);
        err |= sec ? nvs_set_str(h, k, c->email) : erase_key(h, k);
    }
    return err;
}

bool config_any_contact(const cfg_contact_t *contacts)
{
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        if (contacts[i].number[0]) {
            return true;
        }
    }
    return false;
}

bool config_any_dedicated(const cfg_contact_t *contacts)
{
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        if (contacts[i].secret == CFG_SECRET_DEDICATED) {
            return true;
        }
    }
    return false;
}

static void secret_defaults(cfg_secret_t *s)
{
    memset(s, 0, sizeof(*s));
    s->port = APP_SMTP_PORT_TLS;
    s->security = 0; /* SMTP_TLS */
    s->max_s = APP_SECRET_MAX_S_DEFAULT;
    s->cap_hr = APP_SECRET_CAP_DEFAULT;
}

/* Secret settings: every key optional (firmware before 0.6.0 wrote none). */
static void load_secret(nvs_handle_t h, cfg_secret_t *s)
{
    secret_defaults(s);
    get_str(h, "smtp_host", s->host, sizeof(s->host));
    uint16_t v16 = 0;
    if (nvs_get_u16(h, "smtp_port", &v16) == ESP_OK && v16) {
        s->port = v16;
    }
    uint8_t v8 = 0;
    if (nvs_get_u8(h, "smtp_sec", &v8) == ESP_OK && v8 <= 1) {
        s->security = v8;
    }
    get_str(h, "smtp_user", s->user, sizeof(s->user));
    get_str(h, "smtp_pass", s->pass, sizeof(s->pass));
    get_str(h, "smtp_from", s->from_name, sizeof(s->from_name));
    get_str(h, "kid_name", s->kid_name, sizeof(s->kid_name));
    if (nvs_get_u16(h, "sec_max_s", &v16) == ESP_OK && v16 >= APP_SECRET_MAX_S_LO && v16 <= APP_SECRET_MAX_S_HI) {
        s->max_s = v16;
    }
    if (nvs_get_u8(h, "sec_cap_hr", &v8) == ESP_OK && v8 >= APP_SECRET_CAP_LO && v8 <= APP_SECRET_CAP_HI) {
        s->cap_hr = v8;
    }
    v8 = 0;
    nvs_get_u8(h, "sec_bed", &v8);
    s->bed_ok = v8 == 1;
}

static void load(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    mbedtls_platform_zeroize(s_wifi, sizeof(s_wifi));
    s_nwifi = 0;
    s_cfg.max_call_min = APP_CALL_MAX_MIN_DEFAULT;
    s_cfg.bedtime.start_min = APP_BEDTIME_START_DEFAULT;
    s_cfg.bedtime.end_min = APP_BEDTIME_END_DEFAULT;
    pins_defaults(&s_cfg.pins);
    secret_defaults(&s_cfg.secret);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return; /* namespace does not exist yet: empty config */
    }
    /* WiFi slots w0..w4; an empty or missing SSID is skipped, so a gap left
     * by an interrupted write cannot hide the slots after it. */
    s_nwifi = 0;
    for (int i = 0; i < CFG_MAX_WIFI; i++) {
        char k[12];
        cfg_wifi_t *w = &s_wifi[s_nwifi];
        snprintf(k, sizeof(k), "w%d_ssid", i);
        get_str(h, k, w->ssid, sizeof(w->ssid));
        snprintf(k, sizeof(k), "w%d_pass", i);
        get_str(h, k, w->pass, sizeof(w->pass));
        if (w->ssid[0]) {
            s_nwifi++;
        } else {
            mbedtls_platform_zeroize(w, sizeof(*w));
        }
    }
    get_str(h, "sip_domain", s_cfg.sip_domain, sizeof(s_cfg.sip_domain));
    get_str(h, "sip_user", s_cfg.sip_user, sizeof(s_cfg.sip_user));
    get_str(h, "sip_pass", s_cfg.sip_pass, sizeof(s_cfg.sip_pass));
    get_str(h, "caller_id", s_cfg.caller_id, sizeof(s_cfg.caller_id));
    get_str(h, "tw_sid", s_cfg.account_sid, sizeof(s_cfg.account_sid));
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        char k[12];
        snprintf(k, sizeof(k), "c%d_name", i);
        get_str(h, k, s_cfg.contacts[i].name, sizeof(s_cfg.contacts[i].name));
        snprintf(k, sizeof(k), "c%d_num", i);
        get_str(h, k, s_cfg.contacts[i].number, sizeof(s_cfg.contacts[i].number));
        load_sched(h, i, &s_cfg.contacts[i].sched);
        uint8_t sec = CFG_SECRET_OFF;
        snprintf(k, sizeof(k), "c%d_sec", i);
        nvs_get_u8(h, k, &sec);
        snprintf(k, sizeof(k), "c%d_mail", i);
        get_str(h, k, s_cfg.contacts[i].email, sizeof(s_cfg.contacts[i].email));
        /* A mode with no email, or one this firmware does not know, is off. */
        s_cfg.contacts[i].secret = sec <= CFG_SECRET_DEDICATED && s_cfg.contacts[i].email[0] ? sec : CFG_SECRET_OFF;
        if (s_cfg.contacts[i].secret == CFG_SECRET_OFF) {
            s_cfg.contacts[i].email[0] = '\0';
        }
    }
    get_str(h, "tw_key_sid", s_cfg.key_sid, sizeof(s_cfg.key_sid));
    get_str(h, "tw_key_sec", s_cfg.key_secret, sizeof(s_cfg.key_secret));
    get_str(h, "tw_svc", s_cfg.svc_sid, sizeof(s_cfg.svc_sid));
    get_str(h, "tw_env", s_cfg.env_sid, sizeof(s_cfg.env_sid));
    nvs_get_u8(h, "tw_prog", &s_cfg.call_program);
    uint16_t mc = 0;
    if (nvs_get_u16(h, "max_call_min", &mc) == ESP_OK && mc >= APP_CALL_MAX_MIN_LO && mc <= APP_CALL_MAX_MIN_HI) {
        s_cfg.max_call_min = mc;
    }
    /* Bedtime: every key optional (firmware before 0.4.0 wrote none). */
    uint8_t bon = 0;
    nvs_get_u8(h, "bed_on", &bon);
    s_cfg.bedtime.enabled = bon == 1;
    uint16_t bm = 0;
    if (nvs_get_u16(h, "bed_start", &bm) == ESP_OK && bm < 1440) {
        s_cfg.bedtime.start_min = bm;
    }
    if (nvs_get_u16(h, "bed_end", &bm) == ESP_OK && bm < 1440) {
        s_cfg.bedtime.end_min = bm;
    }
    get_str(h, "home_tz", s_cfg.bedtime.tz, sizeof(s_cfg.bedtime.tz));
    get_str(h, "home_tz_name", s_cfg.bedtime.tz_name, sizeof(s_cfg.bedtime.tz_name));
    load_pins(h, &s_cfg.pins);
    load_secret(h, &s_cfg.secret);
    /* Missing (every phone before 0.7.0) or unknown = English. */
    uint8_t lang = PROMPT_LANG_EN;
    if (nvs_get_u8(h, "lang", &lang) == ESP_OK && lang < PROMPT_LANG_COUNT) {
        s_cfg.lang = lang;
    }
    uint8_t p = 0;
    nvs_get_u8(h, "provisioned", &p);
    /* Only trust the flag if the SIP account and a contact are really there. */
    s_cfg.provisioned = p == 1 && s_cfg.sip_domain[0] && s_cfg.sip_user[0] && s_cfg.sip_pass[0] &&
                        config_any_contact(s_cfg.contacts);
    nvs_close(h);
}

static size_t copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = src ? strnlen(src, cap - 1) : 0;
    memcpy(dst, src ? src : "", n);
    dst[n] = '\0';
    return n;
}

/* ---------------------------------------------------------------------------
 * WiFi networks
 * ------------------------------------------------------------------------- */

static int wifi_find_locked(const char *ssid)
{
    for (int i = 0; i < s_nwifi; i++) {
        if (strcmp(s_wifi[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

/* Write slots 0..s_nwifi-1 and erase the rest (caller holds the lock). */
static esp_err_t wifi_save_locked(nvs_handle_t h)
{
    esp_err_t err = ESP_OK;
    for (int i = 0; i < CFG_MAX_WIFI; i++) {
        char ks[12], kp[12];
        snprintf(ks, sizeof(ks), "w%d_ssid", i);
        snprintf(kp, sizeof(kp), "w%d_pass", i);
        if (i < s_nwifi) {
            err |= nvs_set_str(h, ks, s_wifi[i].ssid);
            err |= nvs_set_str(h, kp, s_wifi[i].pass);
        } else {
            esp_err_t e = nvs_erase_key(h, ks);
            err |= e == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : e;
            e = nvs_erase_key(h, kp);
            err |= e == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : e;
        }
    }
    return err;
}

static void wifi_drop_locked(int idx)
{
    memmove(&s_wifi[idx], &s_wifi[idx + 1], sizeof(s_wifi[0]) * (size_t)(s_nwifi - idx - 1));
    s_nwifi--;
    mbedtls_platform_zeroize(&s_wifi[s_nwifi], sizeof(s_wifi[0]));
}

/* Add or update in RAM (caller holds the lock and saves). */
static esp_err_t wifi_add_locked(const char *ssid, const char *pass, const char *evict_except, bool *updated)
{
    int idx = wifi_find_locked(ssid);
    if (updated) {
        *updated = idx >= 0;
    }
    if (idx < 0) {
        if (s_nwifi == CFG_MAX_WIFI) {
            if (!evict_except) {
                return ESP_ERR_NO_MEM;
            }
            int victim = 0;
            while (victim < s_nwifi && strcmp(s_wifi[victim].ssid, evict_except) == 0) {
                victim++;
            }
            ESP_LOGW(TAG, "WiFi list full: forgetting the oldest network \"%s\"", s_wifi[victim].ssid);
            wifi_drop_locked(victim);
        }
        idx = s_nwifi++;
        copy_str(s_wifi[idx].ssid, sizeof(s_wifi[idx].ssid), ssid);
    }
    mbedtls_platform_zeroize(s_wifi[idx].pass, sizeof(s_wifi[idx].pass));
    copy_str(s_wifi[idx].pass, sizeof(s_wifi[idx].pass), pass);
    return ESP_OK;
}

/*
 * Firmware before multi-network support kept one network in wifi_ssid /
 * wifi_pass. Move it into the list (unless it is there already), commit,
 * and only then erase the old keys, so a power cut at any point leaves the
 * network in at least one place and the next boot finishes the job.
 */
static void migrate_legacy_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    cfg_wifi_t old = {0};
    get_str(h, "wifi_ssid", old.ssid, sizeof(old.ssid));
    get_str(h, "wifi_pass", old.pass, sizeof(old.pass));
    size_t n = 0;
    bool any_key = nvs_get_str(h, "wifi_ssid", NULL, &n) == ESP_OK || nvs_get_str(h, "wifi_pass", NULL, &n) == ESP_OK;
    if (!any_key) {
        nvs_close(h);
        return;
    }
    esp_err_t err = ESP_OK;
    lock();
    if (old.ssid[0] && wifi_find_locked(old.ssid) < 0) {
        /* The network the older firmware was using is the newest one. */
        err = wifi_add_locked(old.ssid, old.pass, "", NULL);
        if (err == ESP_OK) {
            err = wifi_save_locked(h);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        s_gen++;
        ESP_LOGI(TAG, "moved the saved WiFi network into the network list (%s)", esp_err_to_name(err));
    }
    unlock();
    if (err == ESP_OK) {
        esp_err_t e1 = nvs_erase_key(h, "wifi_ssid");
        esp_err_t e2 = nvs_erase_key(h, "wifi_pass");
        if ((e1 == ESP_OK || e1 == ESP_ERR_NVS_NOT_FOUND) && (e2 == ESP_OK || e2 == ESP_ERR_NVS_NOT_FOUND)) {
            nvs_commit(h);
        }
    }
    nvs_close(h);
    mbedtls_platform_zeroize(&old, sizeof(old));
}

static bool wifi_args_ok(const char *ssid, const char *pass)
{
    size_t sl = ssid ? strlen(ssid) : 0;
    size_t pl = pass ? strlen(pass) : 0;
    return sl >= 1 && sl <= 32 && pl <= 64;
}

int config_wifi_count(void)
{
    lock();
    int n = s_nwifi;
    unlock();
    return n;
}

int config_wifi_get(cfg_wifi_t *out, int max)
{
    lock();
    int n = s_nwifi < max ? s_nwifi : max;
    memcpy(out, s_wifi, sizeof(s_wifi[0]) * (size_t)n);
    unlock();
    return n;
}

int config_wifi_list(cfg_wifi_info_t *out, int max)
{
    lock();
    int n = s_nwifi < max ? s_nwifi : max;
    for (int i = 0; i < n; i++) {
        strlcpy(out[i].ssid, s_wifi[i].ssid, sizeof(out[i].ssid));
        out[i].secured = s_wifi[i].pass[0] != '\0';
    }
    unlock();
    return n;
}

bool config_wifi_has(const char *ssid)
{
    lock();
    bool r = wifi_find_locked(ssid) >= 0;
    unlock();
    return r;
}

esp_err_t config_wifi_add(const char *ssid, const char *pass, const char *evict_except, bool *updated)
{
    if (!wifi_args_ok(ssid, pass)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    err = wifi_add_locked(ssid, pass ? pass : "", evict_except, updated);
    if (err == ESP_OK) {
        err = wifi_save_locked(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        s_gen++;
    }
    int n = s_nwifi;
    unlock();
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi network \"%s\" saved (%d of %d)", ssid, n, CFG_MAX_WIFI);
    } else if (err != ESP_ERR_NO_MEM) {
        ESP_LOGE(TAG, "saving WiFi network failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t config_wifi_remove(const char *ssid)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    int idx = wifi_find_locked(ssid);
    if (idx < 0) {
        err = ESP_ERR_NOT_FOUND;
    } else if (s_nwifi <= 1) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        wifi_drop_locked(idx);
        err = wifi_save_locked(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        s_gen++;
    }
    unlock();
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi network \"%s\" removed", ssid);
    }
    return err;
}

/* secrets.h.example placeholders are not real values. */
static bool __attribute__((unused)) is_placeholder(const char *v)
{
    return !v[0] || strstr(v, "your-") || strstr(v, "yourname.") || strcmp(v, "sip-credential-password") == 0;
}

/* Seed empty NVS from secrets.h (bench boards). After a factory reset this
 * runs again on the next boot, so a board built with secrets.h comes back
 * configured; build without secrets.h to test the real setup flow. */
static void seed_from_secrets(void)
{
#ifdef WIFI_SSID
    if (config_wifi_count() == 0 && !is_placeholder(WIFI_SSID)) {
        ESP_LOGI(TAG, "seeding WiFi credentials from secrets.h");
        config_wifi_add(WIFI_SSID, WIFI_PASS, NULL, NULL);
    }
#endif
#if defined(SIP_DOMAIN) && defined(SIP_USER) && defined(SIP_PASSWORD) && defined(TEST_DEST_NUMBER)
    if (!s_cfg.provisioned && !is_placeholder(SIP_DOMAIN) && !is_placeholder(SIP_PASSWORD)) {
        ESP_LOGI(TAG, "seeding SIP account and contact 0 from secrets.h");
        cfg_contact_t c = {0};
        copy_str(c.name, sizeof(c.name), "Test");
        copy_str(c.number, sizeof(c.number), TEST_DEST_NUMBER);
#ifdef CALLER_ID
        const char *cid = CALLER_ID;
#else
        const char *cid = "";
#endif
        static cfg_contact_t all[CFG_MAX_CONTACTS]; /* several KB: not on the main task stack */
        all[0] = c;
        config_set_twilio("", SIP_DOMAIN, SIP_USER, SIP_PASSWORD, cid, all);
    }
#endif
}

void config_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    assert(s_lock);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    lock();
    load();
    unlock();
    migrate_legacy_wifi();
    seed_from_secrets();

    const int nw = config_wifi_count();
    ESP_LOGI(TAG, "config: wifi %d network%s%s, twilio %s, numbers key %s", nw, nw == 1 ? "" : "s",
             nw ? "" : " (NOT SET)",
             s_cfg.provisioned ? "provisioned" : "NOT SET UP", config_has_twilio_key() ? "yes" : "no");
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        const char *n = s_cfg.contacts[i].number;
        size_t nl = strlen(n);
        if (nl) {
            ESP_LOGI(TAG, "contact %d: ...%s, schedule %s", i, nl > 4 ? n + nl - 4 : n,
                     s_cfg.contacts[i].sched.enabled ? "on" : "off (always available)");
        }
    }
    ESP_LOGI(TAG, "pins: %u button%s", s_cfg.pins.count, s_cfg.pins.count == 1 ? "" : "s");
    int nsec = 0;
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        nsec += s_cfg.contacts[i].secret != CFG_SECRET_OFF;
    }
    ESP_LOGI(TAG, "secret messages: email %s, %d button%s", s_cfg.secret.host[0] && s_cfg.secret.user[0] &&
             s_cfg.secret.pass[0] ? "set up" : "not set up", nsec, nsec == 1 ? "" : "s");
    if (s_cfg.bedtime.enabled) {
        ESP_LOGI(TAG, "bedtime: %02u:%02u to %02u:%02u, home time zone \"%s\"", s_cfg.bedtime.start_min / 60u,
                 s_cfg.bedtime.start_min % 60u, s_cfg.bedtime.end_min / 60u, s_cfg.bedtime.end_min % 60u,
                 s_cfg.bedtime.tz);
    } else {
        ESP_LOGI(TAG, "bedtime: off");
    }
}

void config_get(app_cfg_t *out)
{
    lock();
    *out = s_cfg;
    unlock();
}

void config_wipe(app_cfg_t *c)
{
    mbedtls_platform_zeroize(c, sizeof(*c));
}

bool config_has_wifi(void)
{
    return config_wifi_count() > 0;
}

bool config_is_provisioned(void)
{
    lock();
    bool r = s_cfg.provisioned;
    unlock();
    return r;
}

uint32_t config_generation(void)
{
    lock();
    uint32_t g = s_gen;
    unlock();
    return g;
}

bool config_get_contact(int idx, cfg_contact_t *out)
{
    if (idx < 0 || idx >= CFG_MAX_CONTACTS) {
        return false;
    }
    lock();
    *out = s_cfg.contacts[idx];
    unlock();
    return out->number[0] != '\0';
}

esp_err_t config_set_twilio(const char *account_sid, const char *sip_domain, const char *sip_user,
                            const char *sip_pass, const char *caller_id, const cfg_contact_t *contacts)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    copy_str(s_cfg.account_sid, sizeof(s_cfg.account_sid), account_sid);
    copy_str(s_cfg.sip_domain, sizeof(s_cfg.sip_domain), sip_domain);
    copy_str(s_cfg.sip_user, sizeof(s_cfg.sip_user), sip_user);
    copy_str(s_cfg.sip_pass, sizeof(s_cfg.sip_pass), sip_pass);
    copy_str(s_cfg.caller_id, sizeof(s_cfg.caller_id), caller_id);
    memcpy(s_cfg.contacts, contacts, sizeof(s_cfg.contacts));
    err |= nvs_set_str(h, "tw_sid", s_cfg.account_sid);
    err |= nvs_set_str(h, "sip_domain", s_cfg.sip_domain);
    err |= nvs_set_str(h, "sip_user", s_cfg.sip_user);
    err |= nvs_set_str(h, "sip_pass", s_cfg.sip_pass);
    err |= nvs_set_str(h, "caller_id", s_cfg.caller_id);
    err |= save_contacts_locked(h);
    err |= nvs_set_u8(h, "provisioned", 1);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    s_cfg.provisioned = err == ESP_OK && config_any_contact(s_cfg.contacts);
    s_gen++;
    unlock();
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving Twilio config failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t config_set_sip_login(const char *sip_user, const char *sip_pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    copy_str(s_cfg.sip_user, sizeof(s_cfg.sip_user), sip_user);
    copy_str(s_cfg.sip_pass, sizeof(s_cfg.sip_pass), sip_pass);
    err |= nvs_set_str(h, "sip_user", s_cfg.sip_user);
    err |= nvs_set_str(h, "sip_pass", s_cfg.sip_pass);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    s_gen++;
    unlock();
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving SIP login failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t config_set_contacts(const cfg_contact_t *contacts)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    memcpy(s_cfg.contacts, contacts, sizeof(s_cfg.contacts));
    err = save_contacts_locked(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    s_cfg.provisioned = s_cfg.provisioned && config_any_contact(s_cfg.contacts);
    s_gen++;
    unlock();
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving contacts failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Set (non-empty) or erase (empty) one string key. */
static esp_err_t put_str(nvs_handle_t h, const char *key, const char *val)
{
    return val[0] ? nvs_set_str(h, key, val) : erase_key(h, key);
}

esp_err_t config_set_twilio_key(const char *key_sid, const char *key_secret, const char *svc_sid,
                                const char *env_sid)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    copy_str(s_cfg.key_sid, sizeof(s_cfg.key_sid), key_sid);
    copy_str(s_cfg.key_secret, sizeof(s_cfg.key_secret), key_secret);
    copy_str(s_cfg.svc_sid, sizeof(s_cfg.svc_sid), svc_sid);
    copy_str(s_cfg.env_sid, sizeof(s_cfg.env_sid), env_sid);
    err |= put_str(h, "tw_key_sid", s_cfg.key_sid);
    err |= put_str(h, "tw_key_sec", s_cfg.key_secret);
    err |= put_str(h, "tw_svc", s_cfg.svc_sid);
    err |= put_str(h, "tw_env", s_cfg.env_sid);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    s_gen++;
    unlock();
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the Twilio key failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t config_set_call_program(uint8_t version)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    err = nvs_set_u8(h, "tw_prog", version);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        s_cfg.call_program = version;
    }
    s_gen++;
    unlock();
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the call program version failed: %s", esp_err_to_name(err));
    }
    return err;
}

uint16_t config_max_call_minutes(void)
{
    lock();
    uint16_t m = s_cfg.max_call_min;
    unlock();
    return m;
}

esp_err_t config_set_max_call_minutes(unsigned minutes)
{
    if (minutes < APP_CALL_MAX_MIN_LO || minutes > APP_CALL_MAX_MIN_HI) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    err = nvs_set_u16(h, "max_call_min", (uint16_t)minutes);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        s_cfg.max_call_min = (uint16_t)minutes;
    }
    s_gen++;
    unlock();
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the call length failed: %s", esp_err_to_name(err));
    }
    return err;
}

uint8_t config_lang(void)
{
    lock();
    uint8_t l = s_cfg.lang;
    unlock();
    return l;
}

esp_err_t config_set_lang(uint8_t lang)
{
    if (lang >= PROMPT_LANG_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    lock();
    err = lang == PROMPT_LANG_EN ? erase_key(h, "lang") : nvs_set_u8(h, "lang", lang);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        s_cfg.lang = lang;
    }
    s_gen++;
    unlock();
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the language failed: %s", esp_err_to_name(err));
    }
    return err;
}

void config_get_bedtime(cfg_bedtime_t *out)
{
    lock();
    *out = s_cfg.bedtime;
    unlock();
}

esp_err_t config_set_bedtime(const cfg_bedtime_t *b)
{
    if (b->start_min >= 1440 || b->end_min >= 1440) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    cfg_bedtime_t nb = {.enabled = b->enabled, .start_min = b->start_min, .end_min = b->end_min};
    copy_str(nb.tz, sizeof(nb.tz), b->tz);
    copy_str(nb.tz_name, sizeof(nb.tz_name), b->tz_name);
    lock();
    err |= nvs_set_u8(h, "bed_on", nb.enabled ? 1 : 0);
    err |= nvs_set_u16(h, "bed_start", nb.start_min);
    err |= nvs_set_u16(h, "bed_end", nb.end_min);
    err |= put_str(h, "home_tz", nb.tz);
    err |= put_str(h, "home_tz_name", nb.tz_name);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        s_cfg.bedtime = nb;
    }
    s_gen++;
    unlock();
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving bedtime failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    return ESP_OK;
}

void config_get_pins(cfg_pins_t *out)
{
    lock();
    *out = s_cfg.pins;
    unlock();
}

esp_err_t config_set_pins(const cfg_pins_t *pins, int *slot)
{
    if (pins->count < 1 || pins->count > CFG_MAX_CONTACTS) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    pins_blob_t b;
    memset(&b, -1, sizeof(b));
    b.version = PINS_BLOB_VERSION;
    b.count = pins->count;
    memcpy(b.button, pins->button, sizeof(pins->button));
    memcpy(b.lamp, pins->lamp, sizeof(pins->lamp));
    lock();
    /* A slot that loses its button must not still call someone or keep a
     * secret mode (checked under the lock, against the contacts as saved
     * right now). */
    for (int i = pins->count; i < CFG_MAX_CONTACTS; i++) {
        if (s_cfg.contacts[i].number[0] || s_cfg.contacts[i].secret != CFG_SECRET_OFF) {
            if (slot) {
                *slot = i;
            }
            unlock();
            nvs_close(h);
            return ESP_ERR_INVALID_STATE;
        }
    }
    err = nvs_set_blob(h, "pins", &b, sizeof(b));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        s_cfg.pins = *pins;
    }
    s_gen++;
    unlock();
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving pins failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    return ESP_OK;
}

void config_get_secret(cfg_secret_t *out)
{
    lock();
    *out = s_cfg.secret;
    unlock();
}

void config_secret_opts(cfg_secret_opts_t *out)
{
    lock();
    const cfg_secret_t *s = &s_cfg.secret;
    out->configured = s->host[0] && s->user[0] && s->pass[0];
    out->bed_ok = s->bed_ok;
    out->cap_hr = s->cap_hr;
    out->max_s = s->max_s;
    unlock();
}

esp_err_t config_set_secret(const cfg_secret_t *in, bool keep_pass)
{
    if (in->max_s < APP_SECRET_MAX_S_LO || in->max_s > APP_SECRET_MAX_S_HI || in->cap_hr < APP_SECRET_CAP_LO ||
        in->cap_hr > APP_SECRET_CAP_HI || in->security > 1 || !in->port) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    cfg_secret_t ns = *in;
    lock();
    if (keep_pass) {
        memcpy(ns.pass, s_cfg.secret.pass, sizeof(ns.pass));
    }
    err |= put_str(h, "smtp_host", ns.host);
    err |= nvs_set_u16(h, "smtp_port", ns.port);
    err |= nvs_set_u8(h, "smtp_sec", ns.security);
    err |= put_str(h, "smtp_user", ns.user);
    if (!keep_pass) {
        err |= put_str(h, "smtp_pass", ns.pass);
    }
    err |= put_str(h, "smtp_from", ns.from_name);
    err |= put_str(h, "kid_name", ns.kid_name);
    err |= nvs_set_u16(h, "sec_max_s", ns.max_s);
    err |= nvs_set_u8(h, "sec_cap_hr", ns.cap_hr);
    err |= ns.bed_ok ? nvs_set_u8(h, "sec_bed", 1) : erase_key(h, "sec_bed");
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        s_cfg.secret = ns;
    }
    s_gen++;
    unlock();
    nvs_close(h);
    mbedtls_platform_zeroize(&ns, sizeof(ns));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the secret message settings failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    return ESP_OK;
}

uint8_t config_slot_secret(int idx, char *email, size_t len)
{
    if (idx < 0 || idx >= CFG_MAX_CONTACTS) {
        if (email && len) {
            email[0] = '\0';
        }
        return CFG_SECRET_OFF;
    }
    lock();
    const uint8_t m = s_cfg.contacts[idx].secret;
    if (email && len) {
        strlcpy(email, s_cfg.contacts[idx].email, len);
    }
    unlock();
    return m;
}

bool config_has_twilio_key(void)
{
    lock();
    bool r = s_cfg.key_sid[0] && s_cfg.key_secret[0] && s_cfg.svc_sid[0] && s_cfg.env_sid[0];
    unlock();
    return r;
}

esp_err_t config_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    lock();
    mbedtls_platform_zeroize(&s_cfg, sizeof(s_cfg));
    mbedtls_platform_zeroize(s_wifi, sizeof(s_wifi));
    s_nwifi = 0;
    s_cfg.max_call_min = APP_CALL_MAX_MIN_DEFAULT;
    s_cfg.bedtime.start_min = APP_BEDTIME_START_DEFAULT;
    s_cfg.bedtime.end_min = APP_BEDTIME_END_DEFAULT;
    pins_defaults(&s_cfg.pins);
    secret_defaults(&s_cfg.secret);
    s_gen++;
    unlock();
    ESP_LOGW(TAG, "factory reset: config erased (%s)", esp_err_to_name(err));
    return err;
}
