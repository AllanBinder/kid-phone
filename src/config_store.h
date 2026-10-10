/*
 * Runtime configuration in NVS (namespace "kidphone").
 *
 *   wifi_ssid, wifi_pass       WiFi station credentials (Improv Serial writes them)
 *   sip_domain, sip_user,      Twilio SIP Domain and credential (provisioning writes them)
 *   sip_pass
 *   caller_id                  the Twilio number calls come from (E.164)
 *   tw_sid                     Twilio Account SID (not a secret; prefills the page)
 *   c0_name..c3_name,          up to CFG_MAX_CONTACTS contacts (E.164 numbers);
 *   c0_num..c3_num             button i calls contact i, an empty number = unused slot
 *   c0_sch..c3_sch             blob, availability schedule per contact (see
 *                              sched_blob_t in config_store.c); missing = always available
 *   provisioned                u8, 1 once Twilio setup finished
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

#define CFG_MAX_CONTACTS 4
#define CFG_MAX_WINDOWS  4

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

typedef struct {
    char name[32];
    char number[20]; /* E.164, "+" and up to 15 digits; "" = unused slot */
    cfg_schedule_t sched;
} cfg_contact_t;

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char sip_domain[96];
    char sip_user[33];
    char sip_pass[65];
    char caller_id[20];
    char account_sid[40];
    cfg_contact_t contacts[CFG_MAX_CONTACTS];
    bool provisioned;
} app_cfg_t;

/* Init NVS flash, load the namespace, seed from secrets.h if present and
 * NVS is empty. Call first in app_main. */
void config_store_init(void);

/* Copy the whole config. Contains secrets: wipe the copy when done if it
 * held the SIP or WiFi password (config_wipe). */
void config_get(app_cfg_t *out);
void config_wipe(app_cfg_t *c);

bool config_has_wifi(void);
bool config_is_provisioned(void);

/* Bumps on every saved change (contacts, schedules, Twilio, WiFi). */
uint32_t config_generation(void);

/* Copy contact idx (name and number). Returns false if idx is empty. */
bool config_get_contact(int idx, cfg_contact_t *out);

/* Persist WiFi credentials. */
esp_err_t config_set_wifi(const char *ssid, const char *pass);

/* Persist a finished Twilio setup (sets provisioned = 1). Exactly
 * CFG_MAX_CONTACTS slots from contacts[0] (empty number = unused slot). */
esp_err_t config_set_twilio(const char *account_sid, const char *sip_domain, const char *sip_user,
                            const char *sip_pass, const char *caller_id, const cfg_contact_t *contacts);

/* Save CFG_MAX_CONTACTS contact slots (names, numbers, schedules) without
 * touching the Twilio account. The caller makes sure the set of numbers
 * still matches what Twilio allows (ALLOWED). */
esp_err_t config_set_contacts(const cfg_contact_t *contacts);

/* True if at least one contact slot has a number. */
bool config_any_contact(const cfg_contact_t *contacts);

/* Erase the whole namespace (WiFi, Twilio, contacts). Caller reboots. */
esp_err_t config_factory_reset(void);
