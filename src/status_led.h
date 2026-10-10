/*
 * Onboard WS2812 RGB LED (APP_LED_GPIO) as a status light, low brightness:
 *   blue           setup needed (WiFi up, Twilio not set up)
 *   blue pulse     Twilio setup running
 *   green          ready (dial tone)
 *   red blinking   no WiFi
 *   white pulse    in a call (calling, ringing, talking)
 *   amber blinking button held long enough for a factory reset warning
 */
#pragma once

#include <stdbool.h>

void status_led_start(void);

/* main.c sets this while the button is held past the test-mode press, as a
 * warning that a factory reset is coming. */
void status_led_reset_warning(bool on);
