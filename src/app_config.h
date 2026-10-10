/*
 * Compile-time configuration for the kid_phone firmware.
 * Credentials, SIP account and contacts are runtime config in NVS
 * (config_store.c); src/secrets.h is only an optional developer override.
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
/* Holding the button this long in any state erases the NVS config (WiFi,
 * Twilio, contacts) and reboots into setup. */
#define APP_FACTORY_RESET_MS    10000

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

/* Status tones (the handset is the phone's status display):
 *   WiFi up, not set up    "setup needed": US reorder, 480 + 620 Hz, 250/250 ms
 *   call failed            US busy, 480 + 620 Hz, 500/500 ms, for APP_BUSY_TONE_MS
 *   no WiFi                slow single beep, 480 Hz, 250 ms every 3 s (quiet,
 *                          so a parent can tell "dead" from "offline") */
#define APP_STATUS_TONE_AMPL    0.08f
#define APP_REORDER_ON_MS       250
#define APP_REORDER_OFF_MS      250
#define APP_BUSY_ON_MS          500
#define APP_BUSY_OFF_MS         500
#define APP_BUSY_TONE_MS        5000
#define APP_NOWIFI_BEEP_HZ      480.0f
#define APP_NOWIFI_ON_MS        250
#define APP_NOWIFI_OFF_MS       2750

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
#define APP_SIP_USER_AGENT      "kid_phone/0.3"

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

/* 1: print every full SIP message at INFO (bring-up). 0 (default): full messages at
 * DEBUG only, first lines still at INFO. Authorization header values are
 * always redacted and the destination number is masked to its last 4 digits. */
#define APP_SIP_LOG_FULL        0

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

/* 1: USB host bring-up logging (HUB/ENUM/USBH debug tags, uac-host debug
 * during open, HPRT root port probe task). 0 for normal use. */
#define APP_USB_DEBUG           0

/* ---------------------------------------------------------------------------
 * Setup: Improv Serial, setup web page, Twilio provisioning, status LED
 * ------------------------------------------------------------------------- */

/* mDNS hostname: the setup page is at http://kidphone.local/ */
#define APP_HOSTNAME            "kidphone"
#define APP_DEVICE_NAME         "Kid Phone"
#define APP_FIRMWARE_NAME       "kid_phone"

/* Improv Serial runs on the console UART (the COM port), same baud as logs. */
#define APP_IMPROV_UART         0
#define APP_IMPROV_RX_BUF       1024
/* How long Improv waits for WiFi to come up after receiving credentials. */
#define APP_IMPROV_CONNECT_MS   20000

/* Setup web server (esp_http_server), port 80, LAN only (station interface). */
#define APP_HTTP_PORT           80
#define APP_HTTP_STACK          6144
#define APP_HTTP_MAX_BODY       1024

/* Twilio provisioning task. TLS + JSON parsing need a big stack. */
#define APP_PROV_STACK          16384
#define APP_PROV_HTTP_TIMEOUT_MS 20000
/* Largest Twilio API response accepted (list pages land in PSRAM). */
#define APP_PROV_MAX_RESP       (256 * 1024)
#define APP_PROV_BUILD_TIMEOUT_MS 120000
#define APP_PROV_BUILD_POLL_MS  3000
/* npm dependencies of the Twilio Function build. voice.js only uses the
 * Twilio global (twiml); the helper library is listed explicitly so the
 * build does not depend on Twilio's implicit defaults. */
#define APP_PROV_FN_DEPS        "[{\"name\":\"twilio\",\"version\":\"5.0.0\"}]"
/* Twilio resource names (rerunning setup finds and reuses these). */
#define APP_TW_NAME             "kid-phone"
#define APP_TW_SIP_USER         "handset1"
#define APP_TW_ENV_NAME         "prod"
#define APP_TW_FN_NAME          "voice"

/* Onboard WS2812 RGB LED (GPIO 48 on this DevKitC clone; some boards use 38). */
#define APP_LED_GPIO            48
/* Peak channel value, 0..255. Low: it is a bedroom object. */
#define APP_LED_BRIGHTNESS      24
