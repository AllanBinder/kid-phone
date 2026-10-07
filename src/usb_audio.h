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
#include <stdint.h>

#include "freertos/FreeRTOS.h"
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
