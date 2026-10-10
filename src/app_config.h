/*
 * Compile-time configuration for the kid_phone firmware (stage 2: SIP call).
 * Credentials and the destination number live in secrets.h (gitignored).
 */
#pragma once

/* Call button: two-lead momentary switch between this GPIO and GND.
 * GPIO 4 is not a strapping pin, not USB (19/20), not octal PSRAM (33-37),
 * not flash (26-32), and not UART0 (43/44). */
#define APP_BUTTON_GPIO         4
#define APP_BUTTON_DEBOUNCE_MS  30
#define APP_BUTTON_POLL_MS      5
/* Holding the button this long while idle cycles the audio test modes A-D. */
#define APP_BUTTON_LONG_MS      1500

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
/* Local ringback while the far end rings (US cadence). */
#define APP_RINGBACK_AMPL       0.10f
#define APP_RINGBACK_ON_MS      2000
#define APP_RINGBACK_OFF_MS     4000

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

/* ---------------------------------------------------------------------------
 * Stage 2: WiFi, SIP, RTP
 * ------------------------------------------------------------------------- */

/* WiFi reconnect delay after a disconnect. */
#define APP_WIFI_RETRY_MS       2000

/* SIP over UDP. One local socket, bound to this port, used for both send
 * and receive. The server is SIP_DOMAIN (secrets.h) on APP_SIP_SERVER_PORT. */
#define APP_SIP_LOCAL_PORT      5060
#define APP_SIP_SERVER_PORT     5060
#define APP_SIP_USER_AGENT      "kid_phone/0.2"

/* RFC 3261 timers. T1 = first retransmit interval, doubling each time;
 * non-INVITE retransmits cap at T2; transactions give up after 64*T1. */
#define APP_SIP_T1_MS           500
#define APP_SIP_T2_MS           4000
#define APP_SIP_TIMEOUT_MS      (64 * APP_SIP_T1_MS)

/* Give up on a call nobody answers: CANCEL after this long. */
#define APP_SIP_RING_TIMEOUT_MS 90000

/* During a call, send a CRLF keepalive to the SIP server this often so the
 * home NAT keeps the SIP mapping open for the remote BYE. */
#define APP_SIP_KEEPALIVE_MS    20000

/* 1: print every full SIP message at INFO (bring-up). 0: full messages at
 * DEBUG only, first lines still at INFO. Authorization header values are
 * always redacted and the destination number is masked to its last 4 digits. */
#define APP_SIP_LOG_FULL        1

/* RTP: local port for the media socket (send and receive, symmetric RTP). */
#define APP_RTP_LOCAL_PORT      10000

/* Receive jitter buffer, in 20 ms PCMU frames. Playout starts once PRIME
 * frames are buffered (and again after an underrun); above MAX the oldest
 * frames are dropped to keep latency bounded. Must be < 16. */
#define APP_JB_PRIME_FRAMES     3
#define APP_JB_MAX_FRAMES       6

/* If the audio task supplies nothing for this long during a call (no
 * handset), the RTP sender emits a silence frame so the NAT pinhole stays open. */
#define APP_RTP_TX_IDLE_MS      60
