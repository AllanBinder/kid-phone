#pragma once

typedef enum {
    MODE_A_WIDEBAND_LOOPBACK = 0,
    MODE_B_TELEPHONE_LOOPBACK,
    MODE_C_DIAL_TONE,
    MODE_D_SILENCE,
    MODE_TEST_COUNT,            /* A-D are the audio test modes (BOOT short press) */
    MODE_CALL = MODE_TEST_COUNT, /* SIP call: mic -> RTP, RTP -> earpiece */
    MODE_RINGBACK,              /* remote is ringing, no early media: local US ringback */
    MODE_SETUP_NEEDED,          /* WiFi up, Twilio not set up: US reorder (fast busy) */
    MODE_BUSY,                  /* a call just failed: US busy for APP_BUSY_TONE_MS */
    MODE_NO_WIFI,               /* no WiFi: slow quiet beep */
    MODE_PROMPT,                /* a spoken prompt (prompts.h), then the idle sound again */
    MODE_COUNT
} app_mode_t;

/* Effective audio mode, derived by main.c from the call state, WiFi state
 * and the test mode selection. Read by the audio task every block. */
app_mode_t app_mode_get(void);
const char *app_mode_name(app_mode_t m);
/* Short label for logs: "A".."D", "CALL", "RING", "SETUP", "BUSY", "NOWIFI", "PROMPT". */
const char *app_mode_short(app_mode_t m);

/* Factory reset (main.c): stop any call, erase the config (NVS namespace and
 * the WiFi driver's copy), reboot into setup. Does not return. source is for
 * the log. Used by the setup page; the BOOT button has its own path that
 * waits for the button to be released before rebooting. */
void app_factory_reset(const char *source);
