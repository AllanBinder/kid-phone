/*
 * Spoken prompts: short clips generated offline (scripts/gen_prompts.py,
 * Piper TTS) and embedded in the firmware as 16 kHz mono 16-bit PCM
 * (src/prompts/<name>.pcm). Nothing is synthesized or downloaded on the device.
 *
 * Any task may start a prompt; the audio task plays it (upsampled to the
 * earpiece rate) while app_mode_get() reports MODE_PROMPT, then the normal
 * idle sound (silence by default, see APP_IDLE_DIAL_TONE) comes back. A new
 * prompt replaces one that is playing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROMPT_RATE 16000

typedef enum {
    PROMPT_NONE = 0,
    PROMPT_TIME_UP,     /* "Time's up! Press the button again to call again." */
    PROMPT_UNAVAILABLE, /* "They can't talk right now. Try again later." */
    PROMPT_NO_ANSWER,   /* "They didn't answer the phone." (twilio/no_answer.wav is the same clip for Twilio) */
    PROMPT_NO_WIFI,     /* "The phone isn't connected. Ask a grown-up for help." */
    PROMPT_NOT_READY,   /* "This phone isn't ready yet. Ask a grown-up for help." */
    PROMPT_CANT_CALL,   /* "Oops, that call didn't work. Let's get a grown-up to help." */
    PROMPT_SLEEP_TIME,  /* "It's sleep time. You can call in the morning." */
    PROMPT_COUNT
} prompt_id_t;

/* Start playing a prompt (PROMPT_NONE does nothing). */
void prompt_play(prompt_id_t id);

/* True from prompt_play() until the audio task has played (or dropped) it. */
bool prompt_active(void);

/* Clip length in milliseconds, 0 for PROMPT_NONE. */
uint32_t prompt_duration_ms(prompt_id_t id);

/* Clip name for logs ("time_up", ...). */
const char *prompt_name(prompt_id_t id);

/* Audio task side. prompt_poll() returns a newly requested prompt (and its
 * request number) or PROMPT_NONE; prompt_done(req) marks that request
 * finished. A request made after it keeps prompt_active() true. */
prompt_id_t prompt_poll(uint32_t *req);
void prompt_done(uint32_t req);

/* Clip data: n little-endian int16 samples at PROMPT_RATE (not necessarily
 * 2-byte aligned, read it bytewise). */
const uint8_t *prompt_clip(prompt_id_t id, size_t *n);
