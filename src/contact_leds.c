#include "contact_leds.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "config_store.h"
#include "pins.h"
#include "schedule.h"
#include "secret.h"
#include "sip.h"
#include "wifi.h"

static const char *TAG = "leds";

static void leds_task(void *arg)
{
    (void)arg;
    while (1) {
        const int64_t ms = esp_timer_get_time() / 1000;
        const bool blink_on = (ms / APP_CONTACT_LED_BLINK_MS) % 2 == 0;
        const int calling = sip_call_contact_index();
        /* No WiFi or not set up: the phone cannot call anyone, so every light
         * is off (a press then says why). */
        const bool can_call = wifi_is_connected() && config_is_provisioned();
        /* Bedtime: every light off, whatever each contact's own hours say.
         * A call already going keeps blinking (it is not cut off). */
        const bool asleep = schedule_bedtime_active();
        /* A secret being recorded blinks its button's light. */
        const int recording = secret_slot();
        /* The pins the button task made live (pins.h); a contact with no
         * light is skipped, it calls all the same. */
        cfg_pins_t pins;
        pins_applied(&pins);
        for (int i = 0; i < pins.count && i < CFG_MAX_CONTACTS; i++) {
            if (pins.lamp[i] < 0) {
                continue;
            }
            bool on;
            if (i == calling || i == recording) {
                on = blink_on;
            } else if (config_slot_secret(i, NULL, 0) == CFG_SECRET_DEDICATED) {
                /* A secret button: on whenever a press would record. */
                on = secret_slot_ready(i, asleep);
            } else {
                cfg_contact_t c;
                const bool has = config_get_contact(i, &c);
                on = can_call && !asleep && has && schedule_available(i);
            }
            gpio_set_level(pins.lamp[i], on ? 1 : 0);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* The pins themselves are set up by pins_apply() (app_main, then the button
 * task on every change). */
void contact_leds_start(void)
{
    xTaskCreatePinnedToCore(leds_task, "contact_leds", 3072, NULL, 1, NULL, 0);
    ESP_LOGI(TAG, "contact lights task started");
}
