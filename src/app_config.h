/*
 * Compile-time configuration for the stage 1 USB audio proof of concept.
 */
#pragma once

/* Mode button: two-lead momentary switch between this GPIO and GND.
 * GPIO 4 is not a strapping pin, not USB (19/20), not octal PSRAM (33-37),
 * not flash (26-32), and not UART0 (43/44). */
#define APP_BUTTON_GPIO         4
#define APP_BUTTON_DEBOUNCE_MS  30
#define APP_BUTTON_POLL_MS      5

/* Audio block size in milliseconds. Every pass of the audio task moves one
 * block. 10 ms is 80 samples at 8 kHz (half of a 20 ms PCMU RTP frame). */
#define APP_BLOCK_MS            10

/* Earpiece volume (0..100, normalized by the driver to the device dB range). */
#define APP_SPK_VOLUME_PCT      60
/* Mic volume, only applied if the device exposes a mic volume control. */
#define APP_MIC_VOLUME_PCT      80

/* US dial tone: 350 Hz + 440 Hz, each tone at this linear amplitude
 * (fraction of full scale). 0.1 each is about -14 dBFS peak for the sum. */
#define APP_DIALTONE_AMPL       0.10f

/* Telephone band low-pass corner used by both decimator and interpolator. */
#define APP_TEL_CUTOFF_HZ       3500.0f

/* Driver ring buffers, in blocks of APP_BLOCK_MS. The TX ring depth sets most
 * of the output latency; the write call blocks when it is full, which is what
 * paces the audio task to the device clock. */
#define APP_TX_RING_BLOCKS      4
#define APP_RX_RING_BLOCKS      8

/* Mic jitter buffer (between USB input and the output path), in blocks.
 * Loopback starts consuming once PRIME blocks are buffered; anything above
 * MAX is dropped (counted as an overrun) to keep latency bounded. */
#define APP_MIC_PRIME_BLOCKS    2
#define APP_MIC_MAX_BLOCKS      6

#define APP_STATS_PERIOD_MS     5000
