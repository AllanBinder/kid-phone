/*
 * Twilio self-provisioning: given the parent's Account SID and Auth Token,
 * create (or find and reuse) everything the phone needs in their own Twilio
 * account, then store the SIP credentials in config_store.
 *
 * Runs in its own task. The Auth Token lives only in that task's RAM and is
 * wiped when the task ends; it is never logged or written to flash.
 * The only hosts contacted are api.twilio.com, serverless.twilio.com and
 * serverless-upload.twilio.com.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "config_store.h"

/* Start provisioning. Copies all inputs (the caller wipes its own copy of the
 * token). contacts = CFG_MAX_CONTACTS slots (empty number = unused), at least
 * one with a number. Returns false if a run is already in progress or the
 * inputs are unusable.
 *
 * numbers_only: the phone is already set up and only the set of numbers
 * changed. Then only ALLOWED is updated and the deployed build redeployed
 * (a few seconds instead of a minute); if anything the quick path needs is
 * missing in Twilio it falls back to the full setup. */
bool twilio_provision_start(const char *account_sid, const char *auth_token, const cfg_contact_t *contacts,
                            bool numbers_only);

/* This phone's SIP login: APP_TW_SIP_USER_PREFIX + last 3 MAC bytes in hex. */
void twilio_sip_username(char *out, size_t len);

bool twilio_provision_running(void);

/* Add the progress of the current or last run to obj:
 *   "state": "idle" | "running" | "done" | "failed"
 *   "steps": [ "Checking your Twilio account...", ... ]
 *   "error": "...", "hint": "..." (failed only) */
void twilio_provision_status_json(cJSON *obj);
