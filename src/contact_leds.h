/*
 * One LED per contact, on the pin picked on the setup page (pins.h, active
 * high; a contact may have none):
 *   on        contact i can be called now (schedule off, inside a window, or
 *             the clock is not synced yet)
 *   off       no number in that slot, outside the contact's available hours,
 *             or bedtime (home time zone, see schedule.h)
 *   blinking  this contact is being called (calling, ringing, talking), or
 *             a secret message for this button is going (secret.h)
 * A dedicated secret button's light is on whenever a press would record a
 * secret instead (secret_slot_ready).
 */
#pragma once

void contact_leds_start(void);
