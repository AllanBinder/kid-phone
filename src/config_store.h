/*
 * Runtime configuration in NVS (namespace "kidphone").
 *
 *   w0_ssid..w4_ssid,          up to CFG_MAX_WIFI saved WiFi networks, in the order
 *   w0_pass..w4_pass           they were added (slots kept packed from w0; an empty
 *                              password = open network). Improv Serial and the setup
 *                              page add them. Older firmware kept one network in
 *                              wifi_ssid / wifi_pass: config_store_init moves it into
 *                              the list once and erases those two keys.
 *   sip_domain, sip_user,      Twilio SIP Domain and credential (provisioning writes them)
 *   sip_pass
 *   caller_id                  the Twilio number calls come from (E.164)
 *   tw_sid                     Twilio Account SID (not a secret; prefills the page)
 *   c0_name..c23_name,         up to CFG_MAX_CONTACTS contacts (E.164 numbers);
 *   c0_num..c23_num            button i calls contact i, an empty number = unused slot.
 *                              Slots 0-3 use the same keys as firmware before 0.5.0;
 *                              an empty slot has no keys at all (missing = empty)
 *   c0_sch..c23_sch            blob, availability schedule per contact (see
 *                              sched_blob_t in config_store.c); missing = always available
 *   pins                       blob, which GPIO is each button and light (see
 *                              pins_blob_t in config_store.c); missing = the 0.4.x
 *                              wiring (APP_BUTTON_GPIOS / APP_CONTACT_LED_GPIOS)
 *   provisioned                u8, 1 once Twilio setup finished
 *   tw_key_sid, tw_key_sec     restricted Twilio API key (SK... and its secret)
 *                              that can only set ALLOWED and redeploy (copy of
 *                              the live build), so a
 *                              number change needs no Auth Token
 *   tw_svc, tw_env             Serverless service and environment SIDs that key updates
 *   tw_prog                    u8, APP_TW_PROGRAM_VERSION of the call program the last
 *                              full setup deployed (missing = 0, older firmware)
 *   max_call_min               u16, longest call in minutes (missing = APP_CALL_MAX_MIN_DEFAULT)
 *   bed_on                     u8, 1 = bedtime on (missing = off, so older configs load unchanged)
 *   bed_start, bed_end         u16, bedtime window in minutes of the day, home time
 *                              (missing = APP_BEDTIME_START_DEFAULT / APP_BEDTIME_END_DEFAULT)
 *   home_tz, home_tz_name      the home time zone (where the child is): POSIX TZ rule and the
 *                              IANA name the page picked it from (missing = not set yet)
 *   c0_sec..c23_sec            u8, secret message mode of slot i (CFG_SECRET_*); missing = off
 *   c0_mail..c23_mail          where slot i's secrets are emailed; only kept while its mode
 *                              is not off (missing = none)
 *   smtp_host, smtp_port,      the parent's mail server for secrets: host, u16 port, u8
 *   smtp_sec                   security (SMTP_TLS = 0, SMTP_STARTTLS = 1, see smtp.h)
 *   smtp_user, smtp_pass       login (an email address, also the From address) and app
 *                              password; the password is never sent back to the page
 *   smtp_from                  display name on the email ("Kid Phone" if missing)
 *   kid_name                   the child's name, used in the subject (missing = none)
 *   sec_max_s                  u16, longest secret in seconds (missing = APP_SECRET_MAX_S_DEFAULT)
 *   sec_cap_hr                 u8, secrets allowed per hour (missing = APP_SECRET_CAP_DEFAULT)
 *   sec_bed                    u8, 1 = secrets allowed at bedtime (missing = no)
 *   lang                       u8, language of the spoken prompts (prompt_lang_t: 1 = Catalan,
 *                              2 = Spanish); missing = English (0 is erased, not written)
 * Every secret key is optional: a phone set up before 0.6.0 has none, and
 * missing keys mean the feature is off. Empty values are erased, not written
 * (24 slots of empty keys would fill the small NVS partition).
 *
 * The Twilio Auth Token is never stored. An optional src/secrets.h (developer
 * override) seeds empty NVS at boot so a bench board keeps working.
 * All getters copy out of a RAM cache under a mutex; any task may call them.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "app_config.h"

/* One contact per button, and every assignable GPIO can be a button. */
#define CFG_MAX_CONTACTS APP_NUM_ASSIGNABLE
#define CFG_MAX_WINDOWS  4
#define CFG_MAX_WIFI     5

/* One saved WiFi network. ssid is raw bytes (UTF-8, 1..32), pass is empty
 * (open network) or the WPA passphrase. */
typedef struct {
    char ssid[33];
    char pass[65];
} cfg_wifi_t;

/* A saved network without its password, for the setup page. */
typedef struct {
    char ssid[33];
    bool secured; /* a password is saved */
} cfg_wifi_info_t;

/* One availability window in the contact's own time zone. It starts on each
 * day set in `days` (bit 0 = Sunday .. bit 6 = Saturday) at start_min and
 * lasts until end_min; end_min <= start_min wraps past midnight into the next
 * day (22:00-01:00), and end_min == start_min means 24 hours. */
typedef struct {
    uint8_t days;
    uint16_t start_min; /* 0..1439 */
    uint16_t end_min;   /* 0..1439 */
} cfg_window_t;

typedef struct {
    bool enabled;       /* false: always available */
    char tz[48];        /* POSIX TZ rule, e.g. "EST5EDT,M3.2.0,M11.1.0" */
    char tz_name[40];   /* IANA name the page picked it from, for display only */
    uint8_t nwin;
    cfg_window_t win[CFG_MAX_WINDOWS];
} cfg_schedule_t;

/* Bedtime: one nightly window in the home time zone, every day. Inside it
 * the lights are off and a press says it is sleep time instead of calling.
 * end_min < start_min wraps past midnight (19:30-07:00); start_min ==
 * end_min is refused (schedule_bedtime_validate). */
typedef struct {
    bool enabled;       /* false (default): no bedtime */
    uint16_t start_min; /* 0..1439, home time */
    uint16_t end_min;   /* 0..1439, home time */
    char tz[48];        /* home time zone, POSIX TZ rule; "" = not set yet */
    char tz_name[40];   /* IANA name the page picked it from, for display only */
} cfg_bedtime_t;

/* Secret message mode of a slot (NVS c<i>_sec). */
#define CFG_SECRET_OFF       0 /* the button only calls (acts on press, as before 0.6.0) */
#define CFG_SECRET_HOLD      1 /* short press calls (on release), a long press records a secret */
#define CFG_SECRET_DEDICATED 2 /* a press records a secret; no number, never calls */

typedef struct {
    char name[32];
    char number[20]; /* E.164, "+" and up to 15 digits; "" = no number (unused, or a dedicated secret button) */
    cfg_schedule_t sched;
    uint8_t secret;  /* CFG_SECRET_*; not off only with an email */
    char email[64];  /* where this slot's secrets go; "" with secret off */
} cfg_contact_t;

/* Secret messages, phone-wide: the parent's own mail account and the limits.
 * configured = host, user and password all set (config_secret_opts). */
typedef struct {
    char host[64];
    uint16_t port;
    uint8_t security;   /* SMTP_TLS (0, implicit TLS) or SMTP_STARTTLS (1), see smtp.h */
    char user[96];      /* login, an email address; also the From address */
    char pass[72];      /* app password; never leaves the phone */
    char from_name[48]; /* display name on the email, "" = APP_SECRET_FROM_DEFAULT */
    char kid_name[32];  /* in the subject, "" = none */
    uint16_t max_s;     /* APP_SECRET_MAX_S_LO..APP_SECRET_MAX_S_HI */
    uint8_t cap_hr;     /* APP_SECRET_CAP_LO..APP_SECRET_CAP_HI */
    bool bed_ok;        /* secrets allowed during bedtime */
} cfg_secret_t;

/* The parts of cfg_secret_t the buttons and lights need, without the login. */
typedef struct {
    bool configured;
    bool bed_ok;
    uint8_t cap_hr;
    uint16_t max_s;
} cfg_secret_opts_t;

/* Which GPIO is each button and its light. Buttons fill slots 0..count-1
 * (slot i = button i = contact i); lamp[i] = -1 for a button with no light.
 * Every pin is in APP_ASSIGNABLE_GPIOS and used once (pins_validate). */
typedef struct {
    uint8_t count;                    /* 1..CFG_MAX_CONTACTS buttons */
    int8_t button[CFG_MAX_CONTACTS];  /* GPIO, -1 from count on */
    int8_t lamp[CFG_MAX_CONTACTS];    /* GPIO or -1 (no light) */
} cfg_pins_t;

typedef struct {
    char sip_domain[96];
    char sip_user[33];
    char sip_pass[65];
    char caller_id[20];
    char account_sid[40];
    cfg_contact_t contacts[CFG_MAX_CONTACTS];
    bool provisioned;
    /* Restricted "numbers" key. key_sid alone (no secret) = a key that no
     * longer works and should be deleted when the next token run replaces it. */
    char key_sid[40];
    char key_secret[40];
    char svc_sid[40];
    char env_sid[40];
    uint8_t call_program;  /* tw_prog: APP_TW_PROGRAM_VERSION last deployed, 0 = unknown */
    uint16_t max_call_min; /* APP_CALL_MAX_MIN_LO..APP_CALL_MAX_MIN_HI */
    cfg_bedtime_t bedtime;
    cfg_pins_t pins;
    cfg_secret_t secret;
    uint8_t lang;          /* prompt_lang_t, PROMPT_LANG_EN if missing or unknown */
} app_cfg_t;

/* Init NVS flash, load the namespace, seed from secrets.h if present and
 * NVS is empty. Call first in app_main. */
void config_store_init(void);

/* Copy the whole config (WiFi networks are kept apart, see config_wifi_*).
 * Contains secrets: wipe the copy when done if it held the SIP password or
 * the key secret (config_wipe). It is several KB: keep it on the heap, not
 * on a task stack. */
void config_get(app_cfg_t *out);
void config_wipe(app_cfg_t *c);

/* True if at least one WiFi network is saved. */
bool config_has_wifi(void);
bool config_is_provisioned(void);

/* Bumps on every saved change (contacts, schedules, Twilio, WiFi). */
uint32_t config_generation(void);

/* Copy contact idx (name and number). Returns false if idx is empty. */
bool config_get_contact(int idx, cfg_contact_t *out);

/* Number of saved WiFi networks (0..CFG_MAX_WIFI). */
int config_wifi_count(void);

/* Copy the saved networks with their passwords, oldest first. Returns the
 * count. Wipe the copy when done (mbedtls_platform_zeroize). */
int config_wifi_get(cfg_wifi_t *out, int max);

/* The saved networks without passwords, oldest first. Returns the count. */
int config_wifi_list(cfg_wifi_info_t *out, int max);

/* True if this SSID is saved. */
bool config_wifi_has(const char *ssid);

/* Add a network, or change the password of a saved one with the same SSID
 * (it keeps its place). ssid must be 1..32 bytes, pass 0..64. A new SSID
 * with CFG_MAX_WIFI already saved: evict_except NULL answers ESP_ERR_NO_MEM;
 * otherwise the oldest saved network whose SSID is not evict_except is
 * dropped to make room (Improv: the browser installer must always work).
 * *updated (may be NULL) says whether the SSID was already saved. */
esp_err_t config_wifi_add(const char *ssid, const char *pass, const char *evict_except, bool *updated);

/* Forget a saved network. ESP_ERR_NOT_FOUND if it is not saved,
 * ESP_ERR_INVALID_STATE if it is the only one (the phone could never get
 * back on WiFi; a factory reset is the way to start over). */
esp_err_t config_wifi_remove(const char *ssid);

/* Persist a finished Twilio setup (sets provisioned = 1). Exactly
 * CFG_MAX_CONTACTS slots from contacts[0] (empty number = unused slot). */
esp_err_t config_set_twilio(const char *account_sid, const char *sip_domain, const char *sip_user,
                            const char *sip_pass, const char *caller_id, const cfg_contact_t *contacts);

/* Save just the SIP login, the moment Twilio accepts a new password, so a
 * setup run that stops later cannot leave the phone with a stale password. */
esp_err_t config_set_sip_login(const char *sip_user, const char *sip_pass);

/* Save CFG_MAX_CONTACTS contact slots (names, numbers, schedules) without
 * touching the Twilio account. The caller makes sure the set of numbers
 * still matches what Twilio allows (ALLOWED). */
esp_err_t config_set_contacts(const cfg_contact_t *contacts);

/* Persist the restricted Twilio key and the service / environment it
 * updates. Empty strings clear a field (all empty = no key). */
esp_err_t config_set_twilio_key(const char *key_sid, const char *key_secret, const char *svc_sid,
                                const char *env_sid);

/* Record which call program (APP_TW_PROGRAM_VERSION) a full setup deployed. */
esp_err_t config_set_call_program(uint8_t version);

/* True if a complete key (sid, secret, service, environment) is stored. */
bool config_has_twilio_key(void);

/* Longest call in minutes, and setting it (ESP_ERR_INVALID_ARG outside
 * APP_CALL_MAX_MIN_LO..APP_CALL_MAX_MIN_HI). */
uint16_t config_max_call_minutes(void);
esp_err_t config_set_max_call_minutes(unsigned minutes);

/* Language of the spoken prompts (prompt_lang_t), and setting it
 * (ESP_ERR_INVALID_ARG if out of range). The caller also hands it to
 * prompt_set_lang(). */
uint8_t config_lang(void);
esp_err_t config_set_lang(uint8_t lang);

/* Bedtime and the home time zone, and saving them (device only, no Twilio).
 * The caller validates first (schedule_bedtime_validate). */
void config_get_bedtime(cfg_bedtime_t *out);
esp_err_t config_set_bedtime(const cfg_bedtime_t *b);

/* The button and light pins (defaults if none saved), and saving them. The
 * caller validates first (pins_validate). ESP_ERR_INVALID_STATE (nothing
 * saved) if a slot that would lose its button still has a number (its
 * contact would be unreachable but still allowed in Twilio) or a secret
 * mode. *slot (may be NULL) is then that slot. */
void config_get_pins(cfg_pins_t *out);
esp_err_t config_set_pins(const cfg_pins_t *pins, int *slot);

/* True if at least one contact slot has a number. */
bool config_any_contact(const cfg_contact_t *contacts);

/* True if at least one slot is a dedicated secret button. */
bool config_any_dedicated(const cfg_contact_t *contacts);

/* Secret messages: the whole setting with the password (wipe the copy when
 * done, mbedtls_platform_zeroize), the parts without it, and saving it
 * (keep_pass: leave the stored password as it is and ignore s->pass). The
 * caller validates first. */
void config_get_secret(cfg_secret_t *out);
void config_secret_opts(cfg_secret_opts_t *out);
esp_err_t config_set_secret(const cfg_secret_t *s, bool keep_pass);

/* Secret mode of slot idx (CFG_SECRET_*), and its email into email (may be
 * NULL). Cheap: no whole-contact copy. */
uint8_t config_slot_secret(int idx, char *email, size_t len);

/* Erase the whole namespace (all WiFi networks, Twilio, Twilio key, contacts, call length, bedtime,
 * pins, secret messages).
 * Caller reboots. */
esp_err_t config_factory_reset(void);
