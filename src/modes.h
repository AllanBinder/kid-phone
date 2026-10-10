#pragma once

typedef enum {
    MODE_A_WIDEBAND_LOOPBACK = 0,
    MODE_B_TELEPHONE_LOOPBACK,
    MODE_C_DIAL_TONE,
    MODE_D_SILENCE,
    MODE_TEST_COUNT,            /* A-D are the audio test modes (long press) */
    MODE_CALL = MODE_TEST_COUNT, /* SIP call: mic -> RTP, RTP -> earpiece */
    MODE_RINGBACK,              /* remote is ringing, no early media: local US ringback */
} app_mode_t;

/* Effective audio mode, derived by main.c from the call state, WiFi state
 * and the test mode selection. Read by the audio task every block. */
app_mode_t app_mode_get(void);
const char *app_mode_name(app_mode_t m);
/* Short label for logs: "A".."D", "CALL" or "RING". */
const char *app_mode_short(app_mode_t m);
