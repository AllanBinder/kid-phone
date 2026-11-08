/*
 * USB host + UAC class driver glue: device discovery, descriptor logging,
 * format selection, open/start/close, and hot-plug handling.
 *
 * The audio engine reads the stream state below under usb_audio_lock().
 * The control task only changes it while holding the same lock, so the
 * engine never touches a handle that is being closed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "usb/uac_host.h"

typedef struct {
    uac_host_device_handle_t handle; /* NULL when not open */
    bool active;                     /* stream started, safe to read/write */
    uint32_t rate;                   /* Hz */
    uint8_t channels;
    uint8_t subframe;                /* bytes per sample per channel */
    uint8_t bits;                    /* valid bits per sample */
} ua_stream_t;

typedef struct {
    ua_stream_t mic;     /* UAC RX (device -> host) */
    ua_stream_t spk;     /* UAC TX (host -> device) */
    uint32_t generation; /* bumped on every open/close, engine re-inits on change */
} ua_state_t;

/* Install USB host + UAC driver and start the control task. */
void usb_audio_start(void);

/* Lock the shared state. Returns false on timeout. */
bool usb_audio_lock(TickType_t timeout);
void usb_audio_unlock(void);

/* Only valid while locked. */
ua_state_t *usb_audio_state(void);

/* True while the handset's earpiece or mic stream is running. */
bool usb_audio_handset_connected(void);

/* Handset troubleshooting (setup page, Advanced). Saved in NVS, applied at
 * the next start: the FIFO split is fixed when the USB host is installed. */
#define HS_LAYOUT_COUNT  2     /* 0 = standard, 1 = bigger earpiece FIFO */
#define HS_FLAG_MIC_OFF  0x01  /* do not open the mic (speaker only test) */
#define HS_FLAG_KEEP_VOL 0x02  /* leave the earpiece volume/mute alone */
#define HS_FLAG_VERBOSE  0x04  /* USB host/UAC debug logging */
#define HS_FLAG_ALL      0x07

void usb_audio_hs_get(uint8_t *layout, uint8_t *flags);
esp_err_t usb_audio_hs_save(uint8_t layout, uint8_t flags);

/* Product name and VID:PID of the last handset seen; false if none yet. */
bool usb_audio_hs_device(char *name, size_t cap, uint16_t *vid, uint16_t *pid);
