/*
 * Twilio self-provisioning: given the parent's Account SID and Auth Token,
 * create (or find and reuse) everything the phone needs in their own Twilio
 * account, then store the SIP credentials in config_store.
 *
 * Runs in its own task. The Auth Token lives only in that task's RAM and is
 * wiped when the task ends; it is never logged or written to flash.
 * Instead, setup leaves a restricted Twilio API key in NVS that can only set
 * the ALLOWED variable and redeploy, so later number changes need no token.
 * The only hosts contacted are api.twilio.com, serverless.twilio.com,
 * serverless-upload.twilio.com and iam.twilio.com.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 * missing in Twilio it falls back to the full setup. Both paths (re)create
 * the restricted numbers key: the full setup always, the quick path when none
 * usable is stored. */
bool twilio_provision_start(const char *account_sid, const char *auth_token, const cfg_contact_t *contacts,
                            bool numbers_only);

/* Numbers-only update with the stored restricted key, no token. Returns
 * false if no key is stored, a run is in progress or the contacts are
 * unusable. If Twilio rejects the key, the run fails with needs_token. */
bool twilio_provision_start_with_key(const cfg_contact_t *contacts);

/* Id of the current or last run (bumps on every start). */
uint32_t twilio_provision_run_id(void);

/* This phone's SIP login: APP_TW_SIP_USER_PREFIX + last 3 MAC bytes in hex. */
void twilio_sip_username(char *out, size_t len);

bool twilio_provision_running(void);

/* Add the progress of the current or last run to obj:
 *   "state": "idle" | "running" | "done" | "failed"
 *   "run": id of the run (see twilio_provision_run_id)
 *   "steps": [ "Checking your Twilio account...", ... ]
 *   "error": "...", "hint": "...", "needs_token": bool (failed only;
 *   needs_token = a key run failed and only the Auth Token can fix it) */
void twilio_provision_status_json(cJSON *obj);
