/*
 * Secret messages: the child records a voice note on the handset and the
 * phone emails it, as a WAV attachment, straight from the parent's own mail
 * account (smtp.h). Nothing passes through Twilio or any server of ours.
 *
 * A slot's secret mode (config_store.h CFG_SECRET_*) decides how a button
 * starts one; main.c does the button side and calls secret_begin(). Then:
 *   1. "Tell your secret after the beep!" (PROMPT_SECRET_START), a beep
 *   2. recording: the mic, downmixed and decimated to 16 kHz mono 16-bit,
 *      into a PSRAM buffer sized for the longest allowed secret (no SIP call,
 *      the handset streams run all the time, usb_audio.c); the button's light
 *      blinks. A press of the same button stops it, so does the length limit
 *      (with a "time's up" beep).
 *   3. under APP_SECRET_MIN_MS: thrown away, nothing said, no email.
 *      Otherwise "Shh! Your secret is on its way." and the recording is
 *      queued for the send task.
 * One secret waits to be sent at a time (held in PSRAM, lost on power loss);
 * while it waits, a new one is refused with the busy tone. The send task
 * tries again with backoff (APP_SECRET_RETRY_*), not during a call.
 *
 * Task use: secret_begin / secret_press / secret_poll from the button task
 * only; secret_audio_* from the audio task only; the rest from any task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/* Outcome of secret_begin(). */
typedef enum {
    SECRET_STARTED = 0,
    SECRET_NOT_SET_UP,  /* no email account saved, or this slot has no email */
    SECRET_NO_WIFI,
    SECRET_BUSY,        /* a secret is being recorded, or one is still waiting to be sent */
    SECRET_CAP,         /* APP_SECRET_CAP per hour reached */
    SECRET_NO_HANDSET,  /* no handset, or a mic rate the recorder cannot use */
    SECRET_NO_MEMORY,
} secret_begin_t;

/* Start the send task. Call after config_store_init(). */
void secret_start(void);

/* Start a secret for slot (the caller has already checked prompt, cooldown,
 * call and bedtime). Plays the start prompt on success. */
secret_begin_t secret_begin(int slot);

/* A press on slot while a secret is going: stops the recording if it is
 * that slot's and recording has started; anything else is ignored. */
void secret_press(int slot);

/* Advance the recording steps. Returns true once when a secret is over
 * (queued or thrown away): the caller starts the press cooldown. */
bool secret_poll(void);

/* True from secret_begin() until secret_poll() reports the secret over. */
bool secret_active(void);

/* Slot whose secret is going (its light blinks), -1 if none. */
int secret_slot(void);

/* A dedicated secret button's light: on when a press would start a secret
 * (email set up, WiFi, nothing waiting to be sent, under the hourly cap, and
 * not bedtime unless secrets are allowed then). */
bool secret_slot_ready(int slot, bool asleep);

/* Audio mode while a secret is going: MODE_BEEP, MODE_SECRET, or -1 (the
 * prompt, or nothing; app_mode_get decides). */
int secret_audio_mode(void);

/* Audio task: n mono samples of the mic at rate (MODE_SECRET only), and a
 * call every pass of the loop (finishes a stop, notices a lost mic). */
void secret_audio_feed(const int16_t *mono, size_t n, uint32_t rate);
void secret_audio_tick(bool have_mic);

/* Setup page: {last: {seq, state, message, slot, seconds, ago_s, utc},
 * test: {seq, state, message}, last_hour (secrets queued in the last hour),
 * waiting (one is waiting to be sent), recording}. state is one of
 * none, recording, queued, sending, retrying, sent, failed, discarded. */
void secret_status_json(cJSON *o);

/* Send a test email (text only) to the login's own address with the saved
 * settings. False (and a message in err) if it cannot start. */
bool secret_test(char *err, size_t err_len);
