#include "tz_posix.h"

#include <ctype.h>
#include <string.h>

/* Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm). */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Year of a day count since 1970-01-01. */
static int64_t year_of_days(int64_t z)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    return yoe + era * 400 + (m <= 2);
}

static bool is_leap(int64_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

/* ---------------------------------------------------------------------------
 * Parsing
 * ------------------------------------------------------------------------- */

static bool parse_name(const char **p)
{
    const char *s = *p;
    if (*s == '<') {
        s++;
        const char *b = s;
        while (*s && *s != '>') {
            if (!isalnum((unsigned char)*s) && *s != '+' && *s != '-') {
                return false;
            }
            s++;
        }
        if (*s != '>' || s - b < 3) {
            return false;
        }
        *p = s + 1;
        return true;
    }
    const char *b = s;
    while (isalpha((unsigned char)*s)) {
        s++;
    }
    if (s - b < 3) {
        return false;
    }
    *p = s;
    return true;
}

static bool parse_num(const char **p, int max, int *out)
{
    const char *s = *p;
    if (!isdigit((unsigned char)*s)) {
        return false;
    }
    int v = 0;
    while (isdigit((unsigned char)*s)) {
        v = v * 10 + (*s - '0');
        if (v > max) {
            return false;
        }
        s++;
    }
    *p = s;
    *out = v;
    return true;
}

/* [+-]hh[:mm[:ss]] in seconds. */
static bool parse_time(const char **p, int max_h, int32_t *out)
{
    const char *s = *p;
    int sign = 1;
    if (*s == '+' || *s == '-') {
        sign = *s == '-' ? -1 : 1;
        s++;
    }
    int h = 0, m = 0, sec = 0;
    if (!parse_num(&s, max_h, &h)) {
        return false;
    }
    if (*s == ':') {
        s++;
        if (!parse_num(&s, 59, &m)) {
            return false;
        }
        if (*s == ':') {
            s++;
            if (!parse_num(&s, 59, &sec)) {
                return false;
            }
        }
    }
    *out = sign * (h * 3600 + m * 60 + sec);
    *p = s;
    return true;
}

static bool parse_rule(const char **p, tz_rule_t *r)
{
    const char *s = *p;
    int v = 0;
    memset(r, 0, sizeof(*r));
    if (*s == 'M') {
        s++;
        int m, w, d;
        if (!parse_num(&s, 12, &m) || m < 1 || *s++ != '.' || !parse_num(&s, 5, &w) || w < 1 || *s++ != '.' ||
            !parse_num(&s, 6, &d)) {
            return false;
        }
        r->kind = 'M';
        r->a = (int16_t)m;
        r->w = (int8_t)w;
        r->d = (int8_t)d;
    } else if (*s == 'J') {
        s++;
        if (!parse_num(&s, 365, &v) || v < 1) {
            return false;
        }
        r->kind = 'J';
        r->a = (int16_t)v;
    } else {
        if (!parse_num(&s, 365, &v)) {
            return false;
        }
        r->kind = 'D';
        r->a = (int16_t)v;
    }
    r->secs = 2 * 3600;
    if (*s == '/') {
        s++;
        if (!parse_time(&s, 167, &r->secs)) {
            return false;
        }
    }
    *p = s;
    return true;
}

bool tz_posix_parse(const char *s, tz_posix_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s || !parse_name(&s) || !parse_time(&s, 24, &out->std_off)) {
        return false;
    }
    if (*s == '\0') {
        return true;
    }
    if (!parse_name(&s)) {
        return false;
    }
    out->has_dst = true;
    out->dst_off = out->std_off - 3600;
    if (*s && *s != ',') {
        if (!parse_time(&s, 24, &out->dst_off)) {
            return false;
        }
    }
    if (*s == '\0') {
        /* No rules given: POSIX leaves this implementation defined. Use the
         * US rules, like glibc and newlib do. */
        out->start = (tz_rule_t){.kind = 'M', .a = 3, .w = 2, .d = 0, .secs = 7200};
        out->end = (tz_rule_t){.kind = 'M', .a = 11, .w = 1, .d = 0, .secs = 7200};
        return true;
    }
    if (*s++ != ',' || !parse_rule(&s, &out->start) || *s++ != ',' || !parse_rule(&s, &out->end)) {
        return false;
    }
    return *s == '\0';
}

/* ---------------------------------------------------------------------------
 * Evaluation
 * ------------------------------------------------------------------------- */

/* Day (days since epoch) on which a rule falls in year y. */
static int64_t rule_day(const tz_rule_t *r, int64_t y)
{
    const int64_t jan1 = days_from_civil(y, 1, 1);
    if (r->kind == 'J') {
        int n = r->a; /* 1..365, Feb 29 never counted */
        return jan1 + n - 1 + (is_leap(y) && n >= 60 ? 1 : 0);
    }
    if (r->kind == 'D') {
        return jan1 + r->a;
    }
    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int dim = mdays[r->a - 1] + (r->a == 2 && is_leap(y) ? 1 : 0);
    const int64_t first = days_from_civil(y, r->a, 1);
    const int wd_first = (int)((first + 4) % 7 + 7) % 7; /* 1970-01-01 was a Thursday */
    int day = 1 + (r->d - wd_first + 7) % 7 + (r->w - 1) * 7;
    while (day > dim) {
        day -= 7;
    }
    return first + day - 1;
}

int32_t tz_posix_utc_offset(const tz_posix_t *tz, int64_t utc)
{
    if (!tz->has_dst) {
        return -tz->std_off;
    }
    const int64_t y = year_of_days(floor_div(utc - tz->std_off, 86400));
    /* Start is given in local standard time, end in local daylight time. */
    const int64_t start = rule_day(&tz->start, y) * 86400 + tz->start.secs + tz->std_off;
    const int64_t end = rule_day(&tz->end, y) * 86400 + tz->end.secs + tz->dst_off;
    bool dst;
    if (start < end) {
        dst = utc >= start && utc < end;          /* northern hemisphere */
    } else {
        dst = !(utc >= end && utc < start);       /* southern hemisphere */
    }
    return -(dst ? tz->dst_off : tz->std_off);
}

void tz_posix_local(const tz_posix_t *tz, int64_t utc, int *wday, int *minute_of_day)
{
    const int64_t local = utc + tz_posix_utc_offset(tz, utc);
    const int64_t days = floor_div(local, 86400);
    const int64_t sod = local - days * 86400;
    *wday = (int)(((days + 4) % 7 + 7) % 7);
    *minute_of_day = (int)(sod / 60);
}
