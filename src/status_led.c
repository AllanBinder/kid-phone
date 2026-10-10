#include "status_led.h"

#include <math.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "app_config.h"
#include "config_store.h"
#include "sip.h"
#include "twilio_provision.h"
#include "wifi.h"

static const char *TAG = "led";

#define LED_PERIOD_MS 40

static led_strip_handle_t s_strip;
static _Atomic bool s_reset_warn;

void status_led_reset_warning(bool on)
{
    atomic_store(&s_reset_warn, on);
}

/* r, g, b are 0..1 scaled to APP_LED_BRIGHTNESS. */
static void set(float r, float g, float b)
{
    const float k = APP_LED_BRIGHTNESS;
    led_strip_set_pixel(s_strip, 0, (uint32_t)(r * k), (uint32_t)(g * k), (uint32_t)(b * k));
    led_strip_refresh(s_strip);
}

static void led_task(void *arg)
{
    (void)arg;
    while (1) {
        const int64_t ms = esp_timer_get_time() / 1000;
        const bool blink = (ms / 500) % 2 == 0;
        const float pulse = 0.15f + 0.85f * (0.5f + 0.5f * sinf((float)(ms % 2000) / 2000.0f * 6.2831853f));
        if (atomic_load(&s_reset_warn)) {
            bool fast = (ms / 150) % 2 == 0;
            set(fast ? 1.0f : 0.0f, fast ? 0.45f : 0.0f, 0.0f);
        } else if (sip_call_state() != CALL_IDLE) {
            set(pulse, pulse, pulse);
        } else if (!wifi_is_connected()) {
            set(blink ? 1.0f : 0.0f, 0.0f, 0.0f);
        } else if (twilio_provision_running()) {
            set(0.0f, 0.0f, pulse);
        } else if (!config_is_provisioned()) {
            set(0.0f, 0.0f, 1.0f);
        } else {
            set(0.0f, 1.0f, 0.0f);
        }
        vTaskDelay(pdMS_TO_TICKS(LED_PERIOD_MS));
    }
}

void status_led_start(void)
{
    const led_strip_config_t cfg = {
        .strip_gpio_num = APP_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "status LED on GPIO %d unavailable: %s", APP_LED_GPIO, esp_err_to_name(err));
        return;
    }
    led_strip_clear(s_strip);
    xTaskCreatePinnedToCore(led_task, "led", 2560, NULL, 1, NULL, 0);
}
