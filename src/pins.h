/*
 * Button and light pins, picked by the parent on the setup page.
 *
 * Which GPIO is button i and which one (if any) is its light is runtime
 * config (cfg_pins_t, NVS "pins"); with nothing saved the phone uses the
 * 0.4.x wiring (APP_BUTTON_GPIOS / APP_CONTACT_LED_GPIOS). Only the pins in
 * APP_ASSIGNABLE_GPIOS (app_config.h, with the reasons) can be picked.
 *
 * The button task owns the GPIO setup: it calls pins_apply() once at start and
 * again whenever the saved pins change, so a Save on the page takes effect
 * within a fraction of a second, no restart. The contact light task drives
 * whatever pins_applied() says is live.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_store.h"

/* The 0.4.x wiring: buttons 4-7, lights 15-18. */
void pins_defaults(cfg_pins_t *out);

/* True if a parent may assign this GPIO (APP_ASSIGNABLE_GPIOS). */
bool pins_is_assignable(int gpio);

/* The assignable GPIOs in page order. Returns the count. */
int pins_assignable(int *out, int max);

/* Check a pin set: 1..CFG_MAX_CONTACTS buttons, every button and light pin
 * assignable, no pin used twice, unused slots cleared. Returns NULL if fine,
 * else a message for the parent (written to err). */
const char *pins_validate(const cfg_pins_t *p, char *err, size_t err_len);

/* Configure the GPIOs for this pin set: buttons as inputs with pull-up,
 * lights as outputs (off), pins no longer used back to disabled with no
 * pulls (so a light that moved goes dark). Returns a mask of the slots whose
 * button pin changed (bit i = slot i), so the caller can restart their
 * debounce. Call from one task only (the button task). */
uint32_t pins_apply(const cfg_pins_t *want);

/* The pin set pins_apply() made live (count 0 before the first apply). */
void pins_applied(cfg_pins_t *out);
