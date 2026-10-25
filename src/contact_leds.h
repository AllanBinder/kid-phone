/*
 * One LED per contact (APP_CONTACT_LED_GPIOS, active high):
 *   on        contact i can be called now (schedule off, inside a window, or
 *             the clock is not synced yet)
 *   off       no number in that slot, outside the contact's available hours,
 *             or bedtime (home time zone, see schedule.h)
 *   blinking  this contact is being called (calling, ringing, talking)
 */
#pragma once

void contact_leds_start(void);
