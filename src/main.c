/*
 * Toddler phone: the button places a real phone call over SIP.
 *
 * ESP32-S3 is USB host (native port, GPIO 19/20) to a USB-C telephone
 * handset, and a SIP client over WiFi to a Twilio SIP Domain. Setup needs no
 * toolchain: WiFi over Improv Serial (browser flasher), then Twilio from the
 * phone's own web page (http://kidphone.local/). One button on APP_BUTTON_GPIO:
 *   idle, short press      call contact 0
 *   in a call, any press   hang up (CANCEL while ringing, BYE when answered)
 *   idle, hold 1.5 s       cycle the stage 1 audio test modes A-D
 *   any state, hold 10 s   factory reset (erase config, reboot into setup)
 * Console is UART0 (the board's COM port), shared with Improv Serial.
 */
#include <stdatomic.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "audio_engine.h"
#include "config_store.h"
#include "improv.h"
#include "modes.h"
#include "rtp.h"
#include "setup_server.h"
#include "sip.h"
#include "status_led.h"
#include "usb_audio.h"
#include "wifi.h"

static const char *TAG = "main";

/* Test mode chosen with long presses while idle, or -1 for the normal idle
 * sound (the status tone, see app_mode_get). */
static _Atomic int s_test_mode = -1;

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
    if (sip_busy_tone_active()) {
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

static void on_short_press_idle(void)
{
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "BUTTON: no WiFi yet, cannot call");
        return;
    }
    if (!config_is_provisioned()) {
        ESP_LOGW(TAG, "BUTTON: not set up yet, open http://%s.local/", APP_HOSTNAME);
        return;
    }
    ESP_LOGI(TAG, "BUTTON: place call");
    atomic_store(&s_test_mode, -1); /* back to dial tone after the call */
    sip_call_start();
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
 * Button: polled, no ISR. Pressed = low (internal pull-up, switch to GND).
 * The level must be stable for APP_BUTTON_DEBOUNCE_MS before it counts.
 * In a call, the press itself hangs up. Idle, the release decides between a
 * short press (call) and a long press (fires at APP_BUTTON_LONG_MS).
 * Held for APP_FACTORY_RESET_MS in any state: factory reset. The LED blinks
 * amber from the long press on as a warning.
 */
static void button_task(void *arg)
{
    (void)arg;
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << APP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    int stable = 1;      /* debounced level, 1 = released */
    int last_raw = 1;
    int same_ms = 0;
    int held_ms = 0;
    bool idle_press = false;  /* current press started while idle */
    bool long_fired = false;
    int any_held_ms = 0;      /* current press, any state (factory reset) */
    while (1) {
        int raw = gpio_get_level(APP_BUTTON_GPIO);
        if (raw == last_raw) {
            if (same_ms < APP_BUTTON_DEBOUNCE_MS) {
                same_ms += APP_BUTTON_POLL_MS;
            }
        } else {
            same_ms = 0;
            last_raw = raw;
        }
        if (same_ms >= APP_BUTTON_DEBOUNCE_MS && raw != stable) {
            stable = raw;
            if (stable == 0) {
                held_ms = 0;
                any_held_ms = 0;
                long_fired = false;
                idle_press = sip_call_state() == CALL_IDLE;
                if (!idle_press) {
                    ESP_LOGI(TAG, "BUTTON: hang up (state %s)", sip_call_state_name(sip_call_state()));
                    sip_call_hangup();
                }
            } else if (idle_press && !long_fired) {
                on_short_press_idle();
            }
        }
        if (stable == 0) {
            any_held_ms += APP_BUTTON_POLL_MS;
            status_led_reset_warning(any_held_ms >= APP_BUTTON_LONG_MS);
            if (any_held_ms >= APP_FACTORY_RESET_MS) {
                factory_reset();
            }
        } else {
            status_led_reset_warning(false);
        }
        if (stable == 0 && idle_press && !long_fired) {
            held_ms += APP_BUTTON_POLL_MS;
            if (held_ms >= APP_BUTTON_LONG_MS) {
                long_fired = true;
                if (sip_call_state() == CALL_IDLE) {
                    cycle_test_mode();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(APP_BUTTON_POLL_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "kid_phone: button places a SIP call");
    ESP_LOGI(TAG, "Button on GPIO %d (to GND): press = call / hang up, hold %d ms while idle = cycle test modes A-D, "
             "hold %d ms = factory reset", APP_BUTTON_GPIO, APP_BUTTON_LONG_MS, APP_FACTORY_RESET_MS);

    config_store_init();
    wifi_start();
    improv_start();
    setup_server_start();
    status_led_start();
    sip_start();
    usb_audio_start();
    audio_engine_start();
    xTaskCreatePinnedToCore(button_task, "button", 3072, NULL, 3, NULL, 0);
}
