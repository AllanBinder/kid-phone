/*
 * Per-contact availability schedules and the clock they need.
 *
 * SNTP (APP_NTP_SERVER_1/2) starts once WiFi is up. Each contact's schedule
 * is evaluated in the contact's own time zone (POSIX TZ rule, see
 * tz_posix.h) from UTC, so the phone's own location does not matter.
 * Until the clock has synced every contact counts as available: clock
 * trouble never blocks a call.
 *
 * A task re-evaluates all contacts every APP_SCHED_EVAL_MS and right after
 * any config change, and caches the result for the contact LEDs.
 */
#pragma once

#include <stdbool.h>

#include "config_store.h"

typedef struct {
    bool available;      /* callable now (schedule off, inside a window, or clock not synced) */
    bool scheduled;      /* schedule enabled with a valid time zone */
    bool has_next;       /* next window start known (only when not available now) */
    int next_wday;       /* 0..6, Sunday = 0, in the contact's time zone */
    int next_minute;     /* minute of the day, contact's time zone */
} sched_state_t;

/* Start SNTP (after WiFi) and the evaluation task. Call after wifi_start(). */
void schedule_start(void);

bool schedule_time_synced(void);

/* Cached availability of contact idx (task updated). */
bool schedule_available(int idx);

/* Evaluate one schedule now (any task). */
void schedule_eval(const cfg_schedule_t *s, sched_state_t *out);

/* Check a schedule from the setup page: time zone parses, windows in range.
 * Returns NULL if fine, else a short reason. */
const char *schedule_validate(const cfg_schedule_t *s);
