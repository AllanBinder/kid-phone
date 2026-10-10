/*
 * Runtime configuration in NVS (namespace "kidphone").
 *
 *   wifi_ssid, wifi_pass       WiFi station credentials (Improv Serial writes them)
 *   sip_domain, sip_user,      Twilio SIP Domain and credential (provisioning writes them)
 *   sip_pass
 *   caller_id                  the Twilio number calls come from (E.164)
 *   c0_name..c3_name,          up to CFG_MAX_CONTACTS contacts (E.164 numbers);
 *   c0_num..c3_num             the button calls contact 0
 *   provisioned                u8, 1 once Twilio setup finished
 *
 * The Twilio Auth Token is never stored. An optional src/secrets.h (developer
 * override) seeds empty NVS at boot so a bench board keeps working.
 * All getters copy out of a RAM cache under a mutex; any task may call them.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define CFG_MAX_CONTACTS 4

typedef struct {
    char name[32];
    char number[20]; /* E.164, "+" and up to 15 digits */
} cfg_contact_t;

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char sip_domain[96];
    char sip_user[33];
    char sip_pass[65];
    char caller_id[20];
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

/* Copy contact idx (name and number). Returns false if idx is empty. */
bool config_get_contact(int idx, cfg_contact_t *out);

/* Persist WiFi credentials. */
esp_err_t config_set_wifi(const char *ssid, const char *pass);

/* Persist a finished Twilio setup (sets provisioned = 1). n contacts are
 * stored from contacts[0], the rest cleared. */
esp_err_t config_set_twilio(const char *sip_domain, const char *sip_user, const char *sip_pass,
                            const char *caller_id, const cfg_contact_t *contacts, int n);

/* Erase the whole namespace (WiFi, Twilio, contacts). Caller reboots. */
esp_err_t config_factory_reset(void);
