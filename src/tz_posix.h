/*
 * POSIX TZ rule strings ("EST5EDT,M3.2.0,M11.1.0", "<+07>-7",
 * "AEST-10AEDT,M10.1.0,M4.1.0/3") evaluated without touching the process-wide
 * TZ environment variable. newlib has a single global TZ, and its setenv()
 * leaks the old string whenever the new value is longer, so per-contact time
 * zones are computed here with plain integer math instead. Pure functions, no
 * ESP-IDF dependency (host-testable).
 *
 * Supported: std/dst names (alphabetic or <quoted>), offsets [+-]hh[:mm[:ss]],
 * optional dst offset (default std + 1 h), transition rules Mm.w.d, Jn and n,
 * each with an optional /[+-]hh[:mm[:ss]] time (default 02:00, hours up to 167).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char kind;      /* 'M', 'J' or 'D' (zero-based day of year) */
    int16_t a;      /* M: month 1..12, J: 1..365, D: 0..365 */
    int8_t w;       /* M: week 1..5 (5 = last) */
    int8_t d;       /* M: weekday 0..6 (0 = Sunday) */
    int32_t secs;   /* transition time of day, local, in seconds */
} tz_rule_t;

typedef struct {
    int32_t std_off; /* POSIX sign: seconds WEST of UTC (local = utc - off) */
    int32_t dst_off;
    bool has_dst;
    tz_rule_t start, end;
} tz_posix_t;

/* Parse a POSIX TZ string. Returns false if it is malformed. */
bool tz_posix_parse(const char *s, tz_posix_t *out);

/* Offset to ADD to UTC to get local time at this instant, in seconds. */
int32_t tz_posix_utc_offset(const tz_posix_t *tz, int64_t utc);

/* Local wall clock: weekday 0..6 (0 = Sunday) and minute of the day 0..1439. */
void tz_posix_local(const tz_posix_t *tz, int64_t utc, int *wday, int *minute_of_day);
