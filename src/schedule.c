#include "schedule.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "tz_posix.h"
#include "wifi.h"

static const char *TAG = "schedule";

#define WEEK_MIN (7 * 1440)
/* Anything before 2025-01-01 means the clock has not been set since boot. */
#define TIME_PLAUSIBLE 1735689600

static _Atomic bool s_synced;
static _Atomic uint32_t s_avail_mask = 0xff; /* bit i = contact i available */

static void on_sync(struct timeval *tv)
{
    (void)tv;
    if (!atomic_exchange(&s_synced, true)) {
        ESP_LOGI(TAG, "clock synced by NTP, UTC %lld", (long long)time(NULL));
    }
}

bool schedule_time_synced(void)
{
    return atomic_load(&s_synced) && time(NULL) > TIME_PLAUSIBLE;
}

bool schedule_available(int idx)
{
    if (idx < 0 || idx >= CFG_MAX_CONTACTS) {
        return false;
    }
    return (atomic_load(&s_avail_mask) >> idx) & 1;
}

static int window_minutes(const cfg_window_t *w)
{
    int d = ((int)w->end_min - (int)w->start_min + 1440) % 1440;
    return d ? d : 1440;
}

const char *schedule_validate(const cfg_schedule_t *s)
{
    if (!s->enabled) {
        return NULL;
    }
    tz_posix_t tz;
    if (!tz_posix_parse(s->tz, &tz)) {
        return "unknown time zone";
    }
    if (s->nwin == 0 || s->nwin > CFG_MAX_WINDOWS) {
        return "add at least one time when they are available";
    }
    for (int i = 0; i < s->nwin; i++) {
        const cfg_window_t *w = &s->win[i];
        if (!(w->days & 0x7f)) {
            return "pick at least one day for each time";
        }
        if (w->start_min >= 1440 || w->end_min >= 1440) {
            return "times must be between 00:00 and 23:59";
        }
    }
    return NULL;
}

void schedule_eval(const cfg_schedule_t *s, sched_state_t *out)
{
    memset(out, 0, sizeof(*out));
    out->available = true;
    tz_posix_t tz;
    if (!s->enabled || s->nwin == 0 || !tz_posix_parse(s->tz, &tz)) {
        return; /* schedule off (or unusable): always available */
    }
    out->scheduled = true;
    if (!schedule_time_synced()) {
        return; /* never block a call because of clock trouble */
    }
    int wday, mod;
    tz_posix_local(&tz, (int64_t)time(NULL), &wday, &mod);
    const int now = wday * 1440 + mod;
    int best = WEEK_MIN;
    int best_start = 0;
    out->available = false;
    for (int i = 0; i < s->nwin && i < CFG_MAX_WINDOWS; i++) {
        const cfg_window_t *w = &s->win[i];
        const int dur = window_minutes(w);
        for (int d = 0; d < 7; d++) {
            if (!(w->days & (1 << d))) {
                continue;
            }
            const int start = d * 1440 + w->start_min;
            /* Minutes since this window opened, on a circular week (wraps
             * past midnight and from Saturday into Sunday). */
            if ((now - start + WEEK_MIN) % WEEK_MIN < dur) {
                out->available = true;
            }
            const int until = (start - now + WEEK_MIN) % WEEK_MIN;
            if (until > 0 && until < best) {
                best = until;
                best_start = start;
            }
        }
    }
    if (!out->available && best < WEEK_MIN) {
        out->has_next = true;
        out->next_wday = best_start / 1440;
        out->next_minute = best_start % 1440;
    }
}

static void start_sntp(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST(APP_NTP_SERVER_1, APP_NTP_SERVER_2));
    cfg.sync_cb = on_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP start failed: %s (schedules stay open until the clock is set)", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SNTP started (%s, %s)", APP_NTP_SERVER_1, APP_NTP_SERVER_2);
}

static void sched_task(void *arg)
{
    (void)arg;
    bool sntp_started = false;
    bool warned_unsynced = false;
    uint32_t last_mask = 0xff;
    app_cfg_t *cfg = malloc(sizeof(app_cfg_t));
    assert(cfg);
    while (1) {
        if (!sntp_started && wifi_is_connected()) {
            start_sntp();
            sntp_started = true;
        }
        config_get(cfg);
        uint32_t mask = 0;
        bool any_scheduled = false;
        for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
            sched_state_t st;
            schedule_eval(&cfg->contacts[i].sched, &st);
            any_scheduled |= st.scheduled;
            if (st.available) {
                mask |= 1u << i;
            }
        }
        config_wipe(cfg); /* it held the SIP password */
        atomic_store(&s_avail_mask, mask);

        const bool synced = schedule_time_synced();
        if (any_scheduled && !synced && !warned_unsynced) {
            ESP_LOGW(TAG, "clock not synced yet: every contact counts as available until it is");
            warned_unsynced = true;
        } else if (synced) {
            warned_unsynced = false;
        }
        if (mask != last_mask) {
            for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
                if (((mask ^ last_mask) >> i) & 1) {
                    ESP_LOGI(TAG, "contact %d %s", i, (mask >> i) & 1 ? "available" : "not available now");
                }
            }
            last_mask = mask;
        }

        /* Re-evaluate every period, or sooner when the config changes. */
        const uint32_t gen = config_generation();
        for (int waited = 0; waited < APP_SCHED_EVAL_MS && config_generation() == gen; waited += 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

void schedule_start(void)
{
    xTaskCreatePinnedToCore(sched_task, "schedule", 4096, NULL, 2, NULL, 0);
}
