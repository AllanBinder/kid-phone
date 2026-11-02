#include "pins.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "app_config.h"

static const char *TAG = "pins";

static const int k_assignable[APP_NUM_ASSIGNABLE] = APP_ASSIGNABLE_GPIOS;
_Static_assert(sizeof(k_assignable) / sizeof(k_assignable[0]) == APP_NUM_ASSIGNABLE,
               "APP_NUM_ASSIGNABLE must match APP_ASSIGNABLE_GPIOS");
_Static_assert(CFG_MAX_CONTACTS <= 32, "slot masks (pins_apply, schedule) are 32 bits");
_Static_assert(APP_DEFAULT_BUTTONS <= CFG_MAX_CONTACTS, "default wiring does not fit");

/* What pins_apply() made live. Written by the button task, read by the light
 * task; the spinlock keeps a copy from tearing. */
static cfg_pins_t s_live;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void pins_defaults(cfg_pins_t *out)
{
    static const int8_t btn[APP_DEFAULT_BUTTONS] = APP_BUTTON_GPIOS;
    static const int8_t led[APP_DEFAULT_BUTTONS] = APP_CONTACT_LED_GPIOS;
    memset(out->button, -1, sizeof(out->button));
    memset(out->lamp, -1, sizeof(out->lamp));
    out->count = APP_DEFAULT_BUTTONS;
    memcpy(out->button, btn, sizeof(btn));
    memcpy(out->lamp, led, sizeof(led));
}

bool pins_is_assignable(int gpio)
{
    for (int i = 0; i < APP_NUM_ASSIGNABLE; i++) {
        if (k_assignable[i] == gpio) {
            return true;
        }
    }
    return false;
}

int pins_assignable(int *out, int max)
{
    int n = max < APP_NUM_ASSIGNABLE ? max : APP_NUM_ASSIGNABLE;
    memcpy(out, k_assignable, sizeof(int) * (size_t)n);
    return n;
}

const char *pins_validate(const cfg_pins_t *p, char *err, size_t err_len)
{
    if (p->count < 1 || p->count > CFG_MAX_CONTACTS) {
        snprintf(err, err_len, "The phone needs 1 to %d buttons.", CFG_MAX_CONTACTS);
        return err;
    }
    uint64_t used = 0;
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        const int b = p->button[i], l = p->lamp[i];
        if (i >= p->count) {
            if (b != -1 || l != -1) {
                snprintf(err, err_len, "Bad pin list.");
                return err;
            }
            continue;
        }
        if (!pins_is_assignable(b)) {
            snprintf(err, err_len, "Button %d: pick a pin for the button.", i + 1);
            return err;
        }
        if (used & (1ULL << b)) {
            snprintf(err, err_len, "Button %d: pin %d is already used by another button or light.", i + 1, b);
            return err;
        }
        used |= 1ULL << b;
        if (l == -1) {
            continue;
        }
        if (!pins_is_assignable(l)) {
            snprintf(err, err_len, "Button %d: pin %d can't be used for a light.", i + 1, l);
            return err;
        }
        if (used & (1ULL << l)) {
            snprintf(err, err_len, "Button %d: pin %d is already used by another button or light.", i + 1, l);
            return err;
        }
        used |= 1ULL << l;
    }
    return NULL;
}

static uint64_t button_mask(const cfg_pins_t *p)
{
    uint64_t m = 0;
    for (int i = 0; i < p->count && i < CFG_MAX_CONTACTS; i++) {
        if (p->button[i] >= 0) {
            m |= 1ULL << p->button[i];
        }
    }
    return m;
}

static uint64_t lamp_mask(const cfg_pins_t *p)
{
    uint64_t m = 0;
    for (int i = 0; i < p->count && i < CFG_MAX_CONTACTS; i++) {
        if (p->lamp[i] >= 0) {
            m |= 1ULL << p->lamp[i];
        }
    }
    return m;
}

uint32_t pins_apply(const cfg_pins_t *want)
{
    cfg_pins_t old;
    pins_applied(&old);
    const uint64_t old_b = button_mask(&old), old_l = lamp_mask(&old);
    const uint64_t new_b = button_mask(want), new_l = lamp_mask(want);

    /* Publish "no lights" first, so the light task stops driving pins that
     * are about to change role; the new set goes live at the end. */
    cfg_pins_t none = *want;
    memset(none.lamp, -1, sizeof(none.lamp));
    portENTER_CRITICAL(&s_mux);
    s_live = none;
    portEXIT_CRITICAL(&s_mux);

    /* Pins that are no longer a button or light: disabled, no pulls. A light
     * driven low first so it is dark at once. */
    const uint64_t released = (old_b | old_l) & ~(new_b | new_l);
    for (int g = 0; g < 64; g++) {
        if (released & (1ULL << g)) {
            if (old_l & (1ULL << g)) {
                gpio_set_level(g, 0);
            }
            gpio_reset_pin(g);
            gpio_set_direction(g, GPIO_MODE_DISABLE);
            gpio_pullup_dis(g);
            gpio_pulldown_dis(g);
        }
    }
    if (new_b & ~old_b) {
        const gpio_config_t cfg = {
            .pin_bit_mask = new_b & ~old_b,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
    }
    if (new_l & ~old_l) {
        const gpio_config_t cfg = {
            .pin_bit_mask = new_l & ~old_l,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        for (int g = 0; g < 64; g++) {
            if ((new_l & ~old_l) & (1ULL << g)) {
                gpio_set_level(g, 0);
            }
        }
    }

    uint32_t changed = 0;
    for (int i = 0; i < CFG_MAX_CONTACTS; i++) {
        const int ob = i < old.count ? old.button[i] : -1;
        const int nb = i < want->count ? want->button[i] : -1;
        if (ob != nb) {
            changed |= 1u << i;
        }
    }
    portENTER_CRITICAL(&s_mux);
    s_live = *want;
    portEXIT_CRITICAL(&s_mux);

    for (int i = 0; i < want->count; i++) {
        if (want->lamp[i] >= 0) {
            ESP_LOGI(TAG, "button %d on GPIO %d, light on GPIO %d", i + 1, want->button[i], want->lamp[i]);
        } else {
            ESP_LOGI(TAG, "button %d on GPIO %d, no light", i + 1, want->button[i]);
        }
    }
    return changed;
}

void pins_applied(cfg_pins_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_live;
    portEXIT_CRITICAL(&s_mux);
}
