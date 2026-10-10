#include "contact_leds.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "config_store.h"
#include "schedule.h"
#include "sip.h"

static const char *TAG = "leds";

_Static_assert(APP_NUM_BUTTONS == CFG_MAX_CONTACTS, "one button and one LED per contact");
static const int k_led_gpio[APP_NUM_BUTTONS] = APP_CONTACT_LED_GPIOS;

static void leds_task(void *arg)
{
    (void)arg;
    while (1) {
        const int64_t ms = esp_timer_get_time() / 1000;
        const bool blink_on = (ms / APP_CONTACT_LED_BLINK_MS) % 2 == 0;
        const int calling = sip_call_contact_index();
        for (int i = 0; i < APP_NUM_BUTTONS && i < CFG_MAX_CONTACTS; i++) {
            cfg_contact_t c;
            bool has = config_get_contact(i, &c);
            bool on;
            if (i == calling) {
                on = blink_on;
            } else {
                on = has && schedule_available(i);
            }
            gpio_set_level(k_led_gpio[i], on ? 1 : 0);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void contact_leds_start(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < APP_NUM_BUTTONS; i++) {
        mask |= 1ULL << k_led_gpio[i];
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    for (int i = 0; i < APP_NUM_BUTTONS; i++) {
        gpio_set_level(k_led_gpio[i], 0);
    }
    xTaskCreatePinnedToCore(leds_task, "contact_leds", 2560, NULL, 1, NULL, 0);
    ESP_LOGI(TAG, "contact LEDs on GPIO %d, %d, %d, %d", k_led_gpio[0], k_led_gpio[1], k_led_gpio[2], k_led_gpio[3]);
}
