/*
 * Compile-time configuration for the kid_phone firmware.
 * Credentials, SIP account and contacts are runtime config in NVS
 * (config_store.c); src/secrets.h is only an optional developer override.
 */
#pragma once

/* Contact buttons and LEDs: button i calls contact i, its light (optional)
 * shows whether contact i can be called right now. The parent picks the pins
 * on the setup page (NVS "pins", see config_store.h and pins.h); a contact
 * without a light still calls, it just has no availability or bedtime light.
 *
 * Buttons: momentary switch between the GPIO and GND, internal pull-up.
 * LEDs: active high, GPIO -> 330 ohm -> LED -> GND (or an LED module with a
 * built-in resistor).
 *
 * Pins a parent may assign on the ESP32-S3 N16R8 DevKitC clone (YD-ESP32-S3),
 * every one broken out on the headers:
 *   1, 2, 4-18, 21, 38-42, 47 (24 pins)
 * Left out:
 *   0, 3, 45, 46   strapping pins (0 is also the BOOT button, APP_BOOT_GPIO)
 *   19, 20         native USB D-/D+, the USB host port for the handset
 *   22-25          do not exist on the ESP32-S3
 *   26-32          SPI flash
 *   33-37          octal PSRAM (N16R8)
 *   43, 44         UART0: console, flashing and Improv Serial
 *   48             onboard WS2812 status LED (APP_LED_GPIO)
 * 39-42 are the pad JTAG pins, but JTAG on this build goes over the USB
 * Serial/JTAG peripheral (eFuse default) and the native USB pins belong to the
 * USB host anyway, so nothing uses pad JTAG and they are plain GPIOs. 15/16
 * are the optional 32 kHz crystal pins, unused on the DevKitC. 38 drives the
 * RGB LED on some other DevKitC boards, but not on this one. Nothing else in
 * the firmware claims a GPIO. Order here is the order the page lists them.
 * CFG_MAX_CONTACTS matches the count, so every pin can be a button (with no
 * lights). */
#define APP_ASSIGNABLE_GPIOS    {1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47}
#define APP_NUM_ASSIGNABLE      24
/* Pins used when NVS has no "pins" entry (every phone set up before 0.5.0,
 * and after a factory reset): four buttons on GPIO 4-7 with lights on 15-18.
 * They sit next to each other on the left header, so the buttons share one
 * run and the LEDs another. */
#define APP_DEFAULT_BUTTONS     4
#define APP_BUTTON_GPIOS        {4, 5, 6, 7}
#define APP_CONTACT_LED_GPIOS   {15, 16, 17, 18}
#define APP_BUTTON_DEBOUNCE_MS  30
#define APP_BUTTON_POLL_MS      5
/* Contact buttons with secret messages off (the default) only call and hang
 * up: one action per press, on the press edge, never on a hold. A button set
 * to "long press" secrets calls on the release instead and starts a secret
 * once held APP_SECRET_HOLD_MS; a dedicated secret button records on a press
 * (main.c). The parent-only controls are on the board's BOOT
 * button instead (GPIO 0, active low, on-board pull-up). GPIO 0 is a
 * strapping pin: it is only read at runtime, never driven, and the reboot
 * after a factory reset waits for it to be released so the chip does not
 * come back up in download mode.
 *   short press (released before APP_BUTTON_LONG_MS), idle: cycle the audio
 *     test modes A-D
 *   hold APP_FACTORY_RESET_MS, any state: erase the NVS config (WiFi,
 *     Twilio, contacts) and reboot into setup; the status LED warns from
 *     APP_BUTTON_LONG_MS on, and releasing before the end cancels */
#define APP_BOOT_GPIO           0
#define APP_BUTTON_LONG_MS      1500
#define APP_FACTORY_RESET_MS    10000
/* After the BOOT reset has erased the config, reboot anyway if the button is
 * still held this long (stuck switch). */
#define APP_BOOT_RELEASE_WAIT_MS 30000
/* Contact LED blink half-period while that contact is being called. */
#define APP_CONTACT_LED_BLINK_MS 300
/* Busy tone length when a button has no contact or the contact is outside
 * their available hours (no call is placed). */
#define APP_SHORT_BUSY_MS       2500
/* Presses are ignored while a spoken prompt plays and for this long after a
 * call ends or fails, so mashing a button cannot place a burst of calls. */
#define APP_PRESS_COOLDOWN_MS   3000

/* Availability schedules: re-evaluated this often (and on config changes). */
#define APP_SCHED_EVAL_MS       1000
/* SNTP servers (the only non-Twilio hosts the phone talks to). Needs
 * CONFIG_LWIP_SNTP_MAX_SERVERS >= 2 in sdkconfig.defaults. */
#define APP_NTP_SERVER_1        "pool.ntp.org"
#define APP_NTP_SERVER_2        "time.google.com"

/* A call that Twilio answers and then hangs up within this long most likely
 * never reached the far end (geo permissions, trial account). twilio/voice.js
 * (NO_ANSWER_PAUSE_S) keeps its "didn't answer" ending longer than this:
 * change both together. */
#define APP_SHORT_CALL_MS       5000

/* Audio block size in milliseconds. Every pass of the audio task moves one
 * block. 10 ms is 80 samples at 8 kHz (half of a 20 ms PCMU RTP frame). */
#define APP_BLOCK_MS            10

/* Earpiece volume (0..100, normalized by the driver to the device dB range). */
#define APP_SPK_VOLUME_PCT      100
/* Mic volume, only applied if the device exposes a mic volume control. */
#define APP_MIC_VOLUME_PCT      80

/* Idle earpiece sound. 0 (default): the earpiece is silent whenever the phone
 * is idle, in every state (ready, no WiFi, not set up), because there is no
 * hook switch yet and a handset on the table would play a tone all day. Sound
 * only follows a press: the spoken prompt, the short busy tone after a failed
 * call or refused press, ringback and the call itself, then silence again.
 * The contact lights show whether the phone can call. 1: the old idle status
 * tones (dial tone when ready, slow beep with no WiFi, reorder when not set
 * up); meant to come back as "dial tone only off-hook" once a hook switch
 * exists. */
#define APP_IDLE_DIAL_TONE      0

/* Tone start, stop and cadence edges ramp over this long (no clicks). */
#define APP_TONE_RAMP_MS        5

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
/* Spoken prompts (16 kHz clips): low-pass corner of the interpolator to the
 * earpiece rate, under the clips' 8 kHz Nyquist limit. */
#define APP_PROMPT_CUTOFF_HZ    7000.0f

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

/* WiFi connection policy (src/wifi.c). At boot and after every disconnect
 * the phone scans all channels, then tries the saved networks it can see,
 * strongest first. A round where none of them works is followed by a pause
 * that starts at BACKOFF_MIN and doubles up to BACKOFF_MAX, then a rescan,
 * forever. ATTEMPT_MS bounds one connect attempt (association, WPA handshake
 * and DHCP); SCAN_TIMEOUT_MS bounds one scan. */
#define APP_WIFI_BACKOFF_MIN_MS 10000
#define APP_WIFI_BACKOFF_MAX_MS 60000
#define APP_WIFI_ATTEMPT_MS     20000
#define APP_WIFI_SCAN_TIMEOUT_MS 10000

/* A call whose WiFi is gone this long, or that finds the phone on a new IP
 * address (another network), is ended locally: its SIP and media addresses
 * are dead. */
#define APP_SIP_NET_LOSS_MS     10000

/* SIP over UDP. One local socket, bound to this port, used for both send
 * and receive. The server is SIP_DOMAIN (secrets.h) on APP_SIP_SERVER_PORT. */
#define APP_SIP_LOCAL_PORT      5060
#define APP_SIP_SERVER_PORT     5060
#define APP_SIP_USER_AGENT      "kid_phone/0.7"

/* RFC 3261 timers. T1 = first retransmit interval, doubling each time;
 * non-INVITE retransmits cap at T2; transactions give up after 64*T1. */
#define APP_SIP_T1_MS           500
#define APP_SIP_T2_MS           4000
#define APP_SIP_TIMEOUT_MS      (64 * APP_SIP_T1_MS)

/* Give up on a call nobody answers: CANCEL after this long. */
#define APP_SIP_RING_TIMEOUT_MS 90000

/* Longest call, in minutes: the phone sends BYE and says "Time's up". The
 * parent sets it on the setup page (NVS "max_call_min"); out-of-range
 * values are refused there and replaced by the default on load. */
#define APP_CALL_MAX_MIN_DEFAULT 30
#define APP_CALL_MAX_MIN_LO     1
#define APP_CALL_MAX_MIN_HI     120

/* Bedtime (parent sets it on the setup page, off by default): one nightly
 * window in the home time zone when the lights are off and a press says
 * "It's sleep time" instead of calling. These are the times the page offers
 * before the parent changes them. */
#define APP_BEDTIME_START_DEFAULT (19 * 60 + 30)
#define APP_BEDTIME_END_DEFAULT   (7 * 60)

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
#define APP_HTTP_STACK          8192
/* Largest request body: a full contact list (CFG_MAX_CONTACTS slots with
 * schedules and secret emails) is about 14 KB. */
#define APP_HTTP_MAX_BODY       20480

/* Twilio provisioning task. TLS + JSON parsing need a big stack. */
#define APP_PROV_STACK          16384
#define APP_PROV_HTTP_TIMEOUT_MS 20000
/* Twilio requests that fail in transport, or get 429 / 5xx, are tried up to
 * this many times in all, waiting APP_PROV_RETRY_MS, 2x, 4x between tries
 * (creates only when they provably never reached Twilio, see
 * twilio_provision.c). */
#define APP_PROV_HTTP_TRIES     4
#define APP_PROV_RETRY_MS       1000
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
/* SIP login of this phone: prefix + last 3 bytes of the WiFi STA MAC in hex
 * (kidphone-a1b2c3), so several phones can share one Twilio account. */
#define APP_TW_SIP_USER_PREFIX  "kidphone-"
/* Login name used by older firmware; provisioning deletes credentials whose
 * username starts with this from the kid-phone credential list. */
#define APP_TW_LEGACY_SIP_USER  "handset1"
#define APP_TW_ENV_NAME         "prod"
#define APP_TW_FN_NAME          "voice"
/* Serverless Assets with the "They didn't answer the phone." clip, one per
 * prompt language: twilio/no_answer.wav as "no-answer", public at
 * https://<env domain>/no-answer.wav, and twilio/no_answer_<lang>.wav as
 * "no-answer-<lang>" at /no-answer-<lang>.wav. The paths must match
 * NO_ANSWER_ASSET in twilio/voice.js. */
#define APP_TW_ASSET_NAME       "no-answer"
/* SIP header with the prompt language ("en", "ca", "es") on every INVITE.
 * Twilio hands X- headers to the Function as event["SipHeader_<name>"]; must
 * match LANG_HEADER in twilio/voice.js. */
#define APP_SIP_LANG_HEADER     "X-Kidphone-Lang"
/* What a full setup installs in Twilio (voice.js and its assets). Bump it
 * whenever twilio/voice.js or an asset changes: a phone whose stored value
 * (NVS tw_prog, 0 = set up before this marker existed) is lower shows "Run
 * setup again" on the setup page. Numbers-only updates deploy a copy of the
 * build that is live and do not change it.
 *   2  (2026-10-18): Dial action, spoken "They didn't answer the phone."
 *   3  (2026-11-02, 0.7.0): no-answer clip and say() lines in the phone's
 *      language (X-Kidphone-Lang header, no-answer-ca / no-answer-es assets) */
#define APP_TW_PROGRAM_VERSION  3

/* ---------------------------------------------------------------------------
 * Secret messages: a recording from the handset mic, emailed by the phone
 * itself over SMTP (secret.c, smtp.c)
 * ------------------------------------------------------------------------- */

/* Hold a "long press" button this long to start a secret (shorter = a call
 * on release). */
#define APP_SECRET_HOLD_MS      1500
/* The beep after "Tell your secret after the beep!", and the "time's up"
 * beep at the length limit. Recording starts APP_SECRET_BEEP_GAP_MS after
 * the beep, so the beep itself is not in it. */
#define APP_SECRET_BEEP_HZ      880.0f
#define APP_SECRET_BEEP_AMPL    0.10f
#define APP_SECRET_BEEP_MS      250
#define APP_SECRET_BEEP_GAP_MS  150
/* Recording format: mono 16-bit at this rate (the mic rate must be a whole
 * multiple of it: 48 kHz, 32 kHz or 16 kHz). Low-pass corner of the
 * decimator in front of it, under the 8 kHz Nyquist limit. */
#define APP_SECRET_RATE         16000
#define APP_SECRET_CUTOFF_HZ    6800.0f
/* Shorter recordings are thrown away (an accidental hold, a quick double
 * press): no email. */
#define APP_SECRET_MIN_MS       2000
/* Longest recording in seconds, set on the setup page (NVS sec_max_s). The
 * buffer for the whole length is taken from PSRAM when a secret starts:
 * 32 KB per second, 3.75 MB at the top of the range. */
#define APP_SECRET_MAX_S_DEFAULT 60
#define APP_SECRET_MAX_S_LO     10
#define APP_SECRET_MAX_S_HI     120
/* Secrets allowed in any 60 minutes (NVS sec_cap_hr); past it a press plays
 * the busy tone. */
#define APP_SECRET_CAP_DEFAULT  5
#define APP_SECRET_CAP_LO       1
#define APP_SECRET_CAP_HI       20
/* Gentle level boost before sending (wav.h): at most this much gain. */
#define APP_SECRET_MAX_GAIN     4.0f
/* Display name on the email when the page leaves it empty. */
#define APP_SECRET_FROM_DEFAULT "Kid Phone"
/* SMTP ports: implicit TLS (the default, as Gmail uses) and STARTTLS. */
#define APP_SMTP_PORT_TLS       465
#define APP_SMTP_PORT_STARTTLS  587
/* Each SMTP step (connect, every reply, every write) gives up after this. */
#define APP_SECRET_SMTP_TIMEOUT_MS 20000
/* Send task. The TLS handshake (certificate chain check, ECDHE) needs a
 * big stack; the mbedtls buffers themselves are in PSRAM
 * (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC). */
#define APP_SECRET_STACK        12288
/* A secret that cannot be sent is tried again after RETRY_MIN, doubling up
 * to RETRY_MAX, for TRIES tries in all (time with no WiFi does not count).
 * Errors a retry cannot fix (login refused, recipient refused, no such
 * server) give up at once. */
#define APP_SECRET_RETRY_MIN_MS 15000
#define APP_SECRET_RETRY_MAX_MS (16 * 60 * 1000)
#define APP_SECRET_TRIES        8

/* Onboard WS2812 RGB LED (GPIO 48 on this DevKitC clone; some boards use 38). */
#define APP_LED_GPIO            48
/* Peak channel value, 0..255. Low: it is a bedroom object. */
#define APP_LED_BRIGHTNESS      24
