#pragma once

typedef enum {
    MODE_A_WIDEBAND_LOOPBACK = 0,
    MODE_B_TELEPHONE_LOOPBACK,
    MODE_C_DIAL_TONE,
    MODE_D_SILENCE,
    MODE_COUNT,
} app_mode_t;

/* Current mode, written by the button task, read by the audio task. */
app_mode_t app_mode_get(void);
void app_mode_next(void);
const char *app_mode_name(app_mode_t m);
char app_mode_letter(app_mode_t m);
