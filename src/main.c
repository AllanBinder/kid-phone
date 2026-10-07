/*
 * Toddler phone, stage 1: USB Audio Class handset proof of concept.
 *
 * ESP32-S3 is USB host (native port, GPIO 19/20) to a USB-C telephone
 * handset. One button on APP_BUTTON_GPIO cycles four audio modes.
 * Console is UART0 (the board's COM port).
 */
#include <stdatomic.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "audio_engine.h"
#include "modes.h"
#include "usb_audio.h"

static const char *TAG = "main";

static _Atomic int s_mode = MODE_C_DIAL_TONE;

app_mode_t app_mode_get(void)
{
    return (app_mode_t)atomic_load(&s_mode);
}

void app_mode_next(void)
{
    int m = (atomic_load(&s_mode) + 1) % MODE_COUNT;
    atomic_store(&s_mode, m);
}

const char *app_mode_name(app_mode_t m)
{
    switch (m) {
    case MODE_A_WIDEBAND_LOOPBACK: return "wideband loopback (mic -> earpiece)";
    case MODE_B_TELEPHONE_LOOPBACK: return "telephone loopback (8 kHz + G.711 mu-law)";
    case MODE_C_DIAL_TONE: return "US dial tone (350 + 440 Hz)";
    case MODE_D_SILENCE: return "silence";
    default: return "?";
    }
}

char app_mode_letter(app_mode_t m)
{
    return (char)('A' + (int)m);
}

/*
 * Button: polled, no ISR. Pressed = low (internal pull-up, switch to GND).
 * The level must be stable for APP_BUTTON_DEBOUNCE_MS before it counts.
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
                app_mode_next();
                app_mode_t m = app_mode_get();
                ESP_LOGI(TAG, "BUTTON: mode -> %c, %s", app_mode_letter(m), app_mode_name(m));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(APP_BUTTON_POLL_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "kid_phone stage 1: USB handset audio test");
    ESP_LOGI(TAG, "Mode button on GPIO %d (to GND). Starting in mode %c, %s",
             APP_BUTTON_GPIO, app_mode_letter(app_mode_get()), app_mode_name(app_mode_get()));

    usb_audio_start();
    audio_engine_start();
    xTaskCreatePinnedToCore(button_task, "button", 3072, NULL, 3, NULL, 0);
}
