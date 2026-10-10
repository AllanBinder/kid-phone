/*
 * Toddler phone: the button places a real phone call over SIP.
 *
 * ESP32-S3 is USB host (native port, GPIO 19/20) to a USB-C telephone
 * handset, and a SIP client over WiFi to a Twilio SIP Domain. Setup needs no
 * toolchain: WiFi over Improv Serial (browser flasher), then Twilio from the
 * phone's own web page (http://kidphone.local/). Up to four contact buttons
 * (APP_BUTTON_GPIOS) with one LED each (APP_CONTACT_LED_GPIOS):
 *   idle, short press i       call contact i (busy tone if the slot is empty
 *                             or the contact is outside their available hours)
 *   in a call, any button     hang up (CANCEL while ringing, BYE when answered)
 *   idle, hold button 0 1.5 s cycle the stage 1 audio test modes A-D
 *   hold button 0 10 s        factory reset (erase config, reboot into setup)
 * Console is UART0 (the board's COM port), shared with Improv Serial.
 */
#include <stdatomic.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "audio_engine.h"
#include "config_store.h"
#include "improv.h"
#include "contact_leds.h"
#include "modes.h"
#include "rtp.h"
#include "schedule.h"
#include "setup_server.h"
#include "sip.h"
#include "status_led.h"
#include "usb_audio.h"
#include "wifi.h"

static const char *TAG = "main";

/* Test mode chosen with long presses while idle, or -1 for the normal idle
 * sound (the status tone, see app_mode_get). */
static _Atomic int s_test_mode = -1;

/* Short busy tone after pressing a button that cannot call (empty slot,
 * contact not available now). */
static _Atomic int64_t s_local_busy_until_ms;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

app_mode_t app_mode_get(void)
{
    call_state_t cs = sip_call_state();
    if (cs == CALL_RINGING && !rtp_media_received()) {
        return MODE_RINGBACK; /* Twilio sends 180 with no media: ring locally */
    }
    if (cs != CALL_IDLE) {
        return MODE_CALL;
    }
    int t = atomic_load(&s_test_mode);
    if (t >= 0) {
        return (app_mode_t)t;
    }
    /* Idle: the handset tells the status. Dial tone only when really ready. */
    if (!wifi_is_connected()) {
        return MODE_NO_WIFI;
    }
    if (sip_busy_tone_active() || now_ms() < atomic_load(&s_local_busy_until_ms)) {
        return MODE_BUSY;
    }
    if (!config_is_provisioned()) {
        return MODE_SETUP_NEEDED;
    }
    return MODE_C_DIAL_TONE;
}

const char *app_mode_name(app_mode_t m)
{
    switch (m) {
    case MODE_A_WIDEBAND_LOOPBACK: return "wideband loopback (mic -> earpiece)";
    case MODE_B_TELEPHONE_LOOPBACK: return "telephone loopback (8 kHz + G.711 mu-law)";
    case MODE_C_DIAL_TONE: return "US dial tone (350 + 440 Hz)";
    case MODE_D_SILENCE: return "silence";
    case MODE_CALL: return "call (mic -> RTP, RTP -> earpiece)";
    case MODE_RINGBACK: return "US ringback (440 + 480 Hz, 2 s on / 4 s off)";
    case MODE_SETUP_NEEDED: return "setup needed (reorder, 480 + 620 Hz, 250/250 ms)";
    case MODE_BUSY: return "busy (480 + 620 Hz, 500/500 ms)";
    case MODE_NO_WIFI: return "no WiFi (slow beep)";
    default: return "?";
    }
}

const char *app_mode_short(app_mode_t m)
{
    static const char *k[] = {"A", "B", "C", "D", "CALL", "RING", "SETUP", "BUSY", "NOWIFI"};
    _Static_assert(sizeof(k) / sizeof(k[0]) == MODE_COUNT, "mode names out of sync");
    return (unsigned)m < MODE_COUNT ? k[m] : "?";
}

/* Long press while idle: auto -> A -> B -> C -> D -> auto. */
static void cycle_test_mode(void)
{
    int t = atomic_load(&s_test_mode) + 1;
    if (t >= MODE_TEST_COUNT) {
        t = -1;
    }
    atomic_store(&s_test_mode, t);
    if (t < 0) {
        ESP_LOGI(TAG, "BUTTON long: test mode off, idle sound = %s", app_mode_name(app_mode_get()));
    } else {
        ESP_LOGI(TAG, "BUTTON long: test mode %s, %s", app_mode_short((app_mode_t)t), app_mode_name((app_mode_t)t));
    }
}

static void short_busy(void)
{
    atomic_store(&s_test_mode, -1);
    atomic_store(&s_local_busy_until_ms, now_ms() + APP_SHORT_BUSY_MS);
}

static void on_short_press_idle(int idx)
{
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "BUTTON %d: no WiFi yet, cannot call", idx);
        return;
    }
    if (!config_is_provisioned()) {
        ESP_LOGW(TAG, "BUTTON %d: not set up yet, open http://%s.local/", idx, APP_HOSTNAME);
        return;
    }
    cfg_contact_t c;
    if (!config_get_contact(idx, &c)) {
        ESP_LOGW(TAG, "BUTTON %d: no contact in this slot, busy tone", idx);
        short_busy();
        return;
    }
    sched_state_t st;
    schedule_eval(&c.sched, &st);
    if (!st.available) {
        ESP_LOGI(TAG, "BUTTON %d: %s is outside their available hours, busy tone", idx, c.name);
        short_busy();
        return;
    }
    if (st.scheduled && !schedule_time_synced()) {
        ESP_LOGW(TAG, "BUTTON %d: clock not synced, calling without checking the schedule", idx);
    }
    ESP_LOGI(TAG, "BUTTON %d: call %s", idx, c.name);
    atomic_store(&s_test_mode, -1); /* back to dial tone after the call */
    atomic_store(&s_local_busy_until_ms, 0);
    sip_call_contact(idx);
}

static void factory_reset(void)
{
    ESP_LOGW(TAG, "BUTTON held %d ms: FACTORY RESET", APP_FACTORY_RESET_MS);
    sip_call_hangup();
    config_factory_reset();
    esp_wifi_restore(); /* the WiFi driver keeps its own copy in NVS */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

/*
 * Buttons: polled, no ISR. Pressed = low (internal pull-up, switch to GND),
 * so an unwired input simply never fires. The level must be stable for
 * APP_BUTTON_DEBOUNCE_MS before it counts. In a call, pressing any button
 * hangs up. Idle, the release decides between a short press (call that
 * contact) and, on button 0 only, a long press (test modes, fires at
 * APP_BUTTON_LONG_MS). Button 0 held for APP_FACTORY_RESET_MS in any state:
 * factory reset; the status LED blinks amber from the long press on.
 */
typedef struct {
    int gpio;
    int stable;      /* debounced level, 1 = released */
    int last_raw;
    int same_ms;
    int held_ms;
    bool idle_press; /* current press started while idle */
    bool long_fired;
} button_t;

static void button_poll(int idx, button_t *b)
{
    int raw = gpio_get_level(b->gpio);
    if (raw == b->last_raw) {
        if (b->same_ms < APP_BUTTON_DEBOUNCE_MS) {
            b->same_ms += APP_BUTTON_POLL_MS;
        }
    } else {
        b->same_ms = 0;
        b->last_raw = raw;
    }
    if (b->same_ms >= APP_BUTTON_DEBOUNCE_MS && raw != b->stable) {
        b->stable = raw;
        if (b->stable == 0) {
            b->held_ms = 0;
            b->long_fired = false;
            b->idle_press = sip_call_state() == CALL_IDLE;
            if (!b->idle_press) {
                ESP_LOGI(TAG, "BUTTON %d: hang up (state %s)", idx, sip_call_state_name(sip_call_state()));
                sip_call_hangup();
            }
        } else if (b->idle_press && !b->long_fired) {
            on_short_press_idle(idx);
        }
    }
    if (b->stable == 0) {
        b->held_ms += APP_BUTTON_POLL_MS;
    }
    if (idx != 0) {
        return;
    }
    /* Button 0 only: test modes and factory reset. */
    if (b->stable == 0) {
        status_led_reset_warning(b->held_ms >= APP_BUTTON_LONG_MS);
        if (b->held_ms >= APP_FACTORY_RESET_MS) {
            factory_reset();
        }
        if (b->idle_press && !b->long_fired && b->held_ms >= APP_BUTTON_LONG_MS) {
            b->long_fired = true;
            if (sip_call_state() == CALL_IDLE) {
                cycle_test_mode();
            }
        }
    } else {
        status_led_reset_warning(false);
    }
}

static void button_task(void *arg)
{
    (void)arg;
    static const int gpios[APP_NUM_BUTTONS] = APP_BUTTON_GPIOS;
    button_t btn[APP_NUM_BUTTONS];
    uint64_t mask = 0;
    for (int i = 0; i < APP_NUM_BUTTONS; i++) {
        btn[i] = (button_t){.gpio = gpios[i], .stable = 1, .last_raw = 1};
        mask |= 1ULL << gpios[i];
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    while (1) {
        for (int i = 0; i < APP_NUM_BUTTONS; i++) {
            button_poll(i, &btn[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(APP_BUTTON_POLL_MS));
    }
}

void app_main(void)
{
    static const int gpios[APP_NUM_BUTTONS] = APP_BUTTON_GPIOS;
    ESP_LOGI(TAG, "kid_phone: button i calls contact i");
    ESP_LOGI(TAG, "Buttons on GPIO %d, %d, %d, %d (to GND): press = call / hang up; button 0: hold %d ms while idle = "
             "cycle test modes A-D, hold %d ms = factory reset", gpios[0], gpios[1], gpios[2], gpios[3],
             APP_BUTTON_LONG_MS, APP_FACTORY_RESET_MS);

    config_store_init();
    wifi_start();
    improv_start();
    setup_server_start();
    status_led_start();
    schedule_start();
    contact_leds_start();
    sip_start();
    usb_audio_start();
    audio_engine_start();
    xTaskCreatePinnedToCore(button_task, "button", 3072, NULL, 3, NULL, 0);
}
