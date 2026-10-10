# kid_phone: a toddler phone that calls family over Twilio

Toddler phone. An ESP32-S3 is USB host to a LIXADA retro telephone handset
(USB Audio Class, "AB17X USB Audio", VID:PID 001f:0b23) and a minimal SIP
client over WiFi to a Twilio SIP Domain in the parent's own Twilio account.
Pick up the handset, hear a dial tone, press a button, and the phone calls
that family member (G.711 mu-law, 8 kHz, RTP over UDP). Up to four buttons,
one per person, each with a light that shows whether that person can be
called right now (optional availability schedule in their own time zone).

A non-technical parent sets it up with no toolchain: flash from a browser,
type the WiFi name and password in the browser, then paste their Twilio
Account SID and Auth Token into a page served by the phone. The phone creates
everything it needs in their Twilio account by itself.

## Setup (for parents)

You need: a computer with Chrome or Edge, a Twilio account with one phone
number that can make calls (Console > Phone Numbers > Buy a number), and the
numbers of the one to four people the phone should call.

1. Plug the phone's **COM** USB port into the computer. Open the installer
   page, press **Connect**, pick the port, choose **Install**.
2. When asked, type your WiFi name and password. The phone joins your WiFi.
3. Press **Visit device** (or open `http://kidphone.local/`). On that page,
   fill in who each button calls (name and number, `+1 555 123 4567` style;
   one person is enough), paste the **Account SID** and **Auth Token** from
   the Twilio Console home page, press **Connect**. Setup takes about a minute.
4. Press **Call ...**, or plug in the handset, pick it up, and press a button.

Later changes: names, which button calls whom, and available times are saved
with **Save** and need no token. Adding or changing a phone number asks for
the Auth Token again, because the Twilio Function only dials numbers on its
allowed list; the phone then updates that list in a few seconds.

**Only when they're available** (per person, optional): pick their time zone
and the times they are happy to take calls (presets "Weekday evenings",
"Weekend mornings", "Every day, daytime", or custom days and times, up to
four; a time like 22:00 to 01:00 runs past midnight). Outside those times
that person's light is off and their button plays a short busy tone instead
of calling. Until the phone's clock has synced from the internet, everyone
counts as available (a clock problem never blocks a call).

If a person is in another country, turn that country on in the Twilio
Console under Voice > Settings > Geo permissions. The phone does not change
that setting. To start over, hold the first button for 10 seconds.

What the handset and the light tell you:

| Earpiece (idle) | Light | Meaning |
|---|---|---|
| slow quiet beep every 3 s | red blinking | no WiFi |
| fast busy (480 + 620 Hz, 250 ms on/off) | blue | WiFi OK, Twilio not set up yet |
| dial tone | green | ready, press a button to call |
| busy (480 + 620 Hz, 500 ms on/off) for 5 s | | the last call failed |
| | blue pulsing | Twilio setup running |
| | white pulsing | in a call |
| | amber fast blink | first button held: keep holding to erase everything |
| busy for 2.5 s after a press | | that button has nobody set up, or the person is outside their available hours |

Contact lights: on = can be called now, off = not set up or outside their
hours, blinking = being called.

## Hardware

- Board: ESP32-S3-DevKitC-1 clone, module N16R8 (16 MB flash, 8 MB octal
  PSRAM; GPIO 33-37 belong to the PSRAM and are not used).
- **COM port** (USB-UART bridge, UART0 on GPIO 43/44): flashing, console,
  logs. Connect this one to the Mac.
- **Native USB port** (GPIO 19/20, labelled USB or OTG): USB host. Plug the
  handset in here.
- **USB-OTG solder pad bridged**: required, so the board's 5 V comes out of
  the native port and powers the handset. On this clone (YD-ESP32-S3 layout)
  there are two pads near the USB-C jacks: **USB-OTG** joins the native jack's
  VBUS to the 5 V rail (this is the one that matters), **IN-OUT** bypasses the
  diode on the 5V header pin (harmless, does nothing for the handset). With
  USB-OTG bridged, never plug the native port into a computer (it would
  back-feed 5 V), and do not feed a second supply into the 5V header pin.
- Symptom of a missing bridge: `HPRT ... conn=0 ... D+=0 D-=0` and no USB
  activity at all, even though the handset works on a computer.
- **Buttons and lights** (`APP_BUTTON_GPIOS`, `APP_CONTACT_LED_GPIOS` in
  `src/app_config.h`). Wire as many as you have people; one is fine, unwired
  buttons never fire (internal pull-up) and unwired lights do nothing.

  | Person | Button (other lead to GND) | Light (GPIO -> 330 ohm -> LED -> GND) |
  |---|---|---|
  | 1 (also test modes and factory reset) | GPIO 4 | GPIO 15 |
  | 2 | GPIO 5 | GPIO 16 |
  | 3 | GPIO 6 | GPIO 17 |
  | 4 | GPIO 7 | GPIO 18 |

  Buttons need no resistor (internal pull-up, 30 ms software debounce).
  Lights are active high: GPIO to a 330 ohm resistor to the LED's long leg
  (anode), short leg to GND. For a solderless build use LED modules with a
  built-in resistor (signal to the GPIO, minus to GND). All eight pins sit
  in one run on the DevKitC's left header (4, 5, 6, 7, 15, 16, 17, 18) and
  avoid strapping pins (0, 3, 45, 46), USB (19/20), UART0 (43/44), flash and
  octal PSRAM (26-37), JTAG (39-42) and the RGB LED (48). 15/16 are the
  optional 32 kHz crystal pins, unused on this board.

## Build, flash, monitor

Pure ESP-IDF 5.5 via PlatformIO (`espressif32@6.13.0`, `framework = espidf`).
The UAC class driver `espressif/usb_host_uac` (1.5.x) is pulled from the ESP
component registry through `src/idf_component.yml` on the first build.

Use the PlatformIO in `~/.platformio/penv` (Python 3.13). The Homebrew `pio`
on this Mac runs on a broken Python 3.14 (its `pyexpat` cannot load), which
makes the ESP-IDF venv setup fail.

```sh
cd ~/Documents/kid_phone

# build only
~/.platformio/penv/bin/pio run

# find the COM port (the USB-UART bridge shows up as cu.usbserial-* or cu.wchusbserial*)
ls /dev/cu.*

# flash over the COM port, then open the monitor (115200 baud)
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/cu.usbserial-XXXX && \
~/.platformio/penv/bin/pio device monitor -p /dev/cu.usbserial-XXXX -b 115200
```

If the upload cannot connect, hold BOOT, tap RST, release BOOT, and retry.
Ctrl-C quits the monitor. After editing `sdkconfig.defaults`, delete
`sdkconfig.esp32s3` so the change is picked up.

`sdkconfig.defaults` adds, among others, `CONFIG_LWIP_MAX_SOCKETS=16`,
`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` and `CONFIG_LWIP_SNTP_MAX_SERVERS=2`
(two NTP servers).

### Browser flasher

`scripts/build_web_bin.sh` merges bootloader, partition table and app into
`web/firmware/kid_phone.bin` (offset 0); see "Web installer" at the end.
ESP Web Tools detects the firmware's Improv Serial and asks for WiFi right
after flashing. A published image must be built without `src/secrets.h`.

## Configuration (developer notes)

All runtime config lives in NVS, namespace `kidphone` (`src/config_store.c`):

| Key | Type | Written by |
|---|---|---|
| `wifi_ssid`, `wifi_pass` | str | Improv Serial (only after the connection works) |
| `sip_domain`, `sip_user`, `sip_pass` | str | Twilio provisioning |
| `caller_id` | str | provisioning (the Twilio number, E.164) |
| `tw_sid` | str | provisioning (Account SID, not a secret; prefills the page and enables the numbers-only update) |
| `c0_name`/`c0_num` .. `c3_name`/`c3_num` | str | provisioning or `POST /api/contacts`; slot i = button i, empty number = unused |
| `c0_sch` .. `c3_sch` | blob, 111 bytes | same; availability schedule, missing = always available |
| `provisioned` | u8 | provisioning (1 = done; trusted only if the SIP account and at least one number exist) |

Schedule blob (`sched_blob_t` in `config_store.c`, packed, little endian):
`version` u8 (1), `enabled` u8, `nwin` u8, `tz` char[48] (POSIX TZ rule),
`tz_name` char[40] (IANA name, display only), then 4 windows of `days` u8
(bit 0 = Sunday .. bit 6 = Saturday), `start_min` u16, `end_min` u16. A
window starts on each set day at `start_min`; `end_min <= start_min` runs
into the next day, equal means 24 hours. A blob with another version or size
is ignored (contact always available).

The Twilio Auth Token is never stored: it lives in the provisioning task's
RAM and is zeroed when the task ends. It is never logged, and no endpoint ever
returns a password. Phone numbers are logged masked to the last 4 digits.

`src/secrets.h` is an **optional developer override** (gitignored, template
`src/secrets.h.example`). If present and NVS is empty, it seeds NVS at boot
(`WIFI_SSID`, `WIFI_PASS`, `SIP_DOMAIN`, `SIP_USER`, `SIP_PASSWORD`,
`TEST_DEST_NUMBER` as contact 0, optional `CALLER_ID`), and
`WIFI_STATIC_IP` / `WIFI_STATIC_GW` still skip DHCP. Placeholder values from
the example are ignored. Because seeding runs on every boot with empty NVS,
a board built with secrets.h comes back configured after a factory reset:
build without it to test the real setup flow. The build works with or
without the file.

### Improv Serial

`src/improv.c` speaks Improv Serial (improv-wifi.com/serial) on UART0, the
COM port, at 115200, the same port as the logs. The UART driver is installed
and stdout is routed through it, so a log line can never split a packet;
outgoing packets are framed as `\n<packet>\n` because the browser only parses
packets at the start of a line. Nothing else reads UART0 RX. Handled RPCs:
WiFi settings (state Provisioning, connect with a 20 s timeout, save to NVS
on success, reply Provisioned with `http://kidphone.local/` and
`http://<ip>/`; on failure error "unable to connect" and the old credentials
are restored), current state, device info (`kid_phone`, app version,
`ESP32-S3`, `Kid Phone`), WiFi scan. Others get "unknown RPC command".

### Setup web page

`esp_http_server` on port 80 plus mDNS `kidphone.local` (`src/setup_server.c`,
page in `src/setup_page.html`, embedded in flash, no external assets). The
phone only has the station interface, so it is only reachable on the home LAN.

| Endpoint | What |
|---|---|
| `GET /` | setup page |
| `GET /api/status` | WiFi, Twilio (provisioned, caller ID, account SID, SIP user, `login_current`), 4 contact slots with schedule, `available`, `next` {day, time} in their time zone, `time_synced`, handset, call state and contact, last call result (`code`, `kind`, `message`), setup progress |
| `POST /api/contacts` | `{contacts:[{name, number, schedule:{enabled, tz, tz_name, windows:[{days, start, end}]}} x up to 4]}`; saves names, button order and schedules without a token. If the set of numbers differs from what Twilio allows: 409 `{needs_token: true}` |
| `POST /api/provision` | `{account_sid, auth_token, contacts, full}` (202). Already set up with its own login, same account, `full` not true: numbers-only update. Otherwise the full setup. `account_sid` may be omitted once stored. Old `contact_name`/`contact_number` still accepted |
| `POST /api/call` | `{contact: i}` test call, same rules as button i (409 if outside their hours) |
| `POST /api/hangup` | hang up |

POST bodies must be `application/json` (a cross-site page cannot send that
without a CORS preflight, which is never answered), and the Host header must
be `kidphone.local` or an IP literal (DNS rebinding). No other auth in v1:
anyone on the LAN can rerun setup or place a test call.

### Twilio provisioning

`src/twilio_provision.c`, its own task (16 KB stack, TLS buffers in PSRAM),
HTTPS with the ESP-IDF certificate bundle, Basic auth SID:token. Only
`api.twilio.com`, `serverless.twilio.com`, `serverless-upload.twilio.com` are
contacted. Every step reuses what an earlier run created, so it is safe to
rerun ("Run setup again" on the page reruns it):

1. `GET Accounts/{SID}.json`: verify credentials, account active (trial noted).
2. `GET IncomingPhoneNumbers.json`: first number with voice = caller ID, else
   "Buy a phone number in Twilio first".
3. Credential list `kid-phone`, credential `kidphone-<last 3 bytes of the
   WiFi STA MAC in hex>` (e.g. `kidphone-a1b2c3`, unique per phone) with a
   fresh random 32-char password (existing passwords cannot be read back, so
   it is updated each run). Credentials whose username starts with
   `handset1` (older firmware) are deleted from that list; other
   `kidphone-*` credentials are other phones on the same account and kept.
4. SIP domain with friendly name `kid-phone`, else
   `kidphone-<6 hex>.sip.twilio.com`; credential list mapped under
   `Auth/Calls/CredentialListMappings` (no registration). If another list
   mapped on the domain (either mapping API) holds the same username, the
   progress log shows a warning: Twilio may then check the wrong password and
   answer 403 after a correct-looking login (what happened on 2026-10-10).
5. Serverless service `kid-phone`, environment `prod`, function `voice`;
   upload `twilio/voice.js` (embedded at build time, one source of truth) as
   `/voice`, protected; build with `APP_PROV_FN_DEPS`; poll until completed
   (2 min timeout); set `CALLER_ID` and `ALLOWED`; deploy.
6. SIP domain `VoiceUrl = https://<env domain>/voice` (POST).
7. Save account SID, SIP domain/user/password, caller ID, contacts with
   schedules, `provisioned=1`.

Numbers-only update (numbers changed on a phone already set up with its own
login): 1, find the `kid-phone` service and `prod` environment, set
`ALLOWED`, redeploy the environment's current `build_sid`, save the
contacts. If anything is missing it falls back to the full run. **Run setup
again** on the page always does the full run (fresh password).

### Availability schedules and the clock

`src/schedule.c`. SNTP (`esp_netif_sntp`, servers `pool.ntp.org` and
`time.google.com`) starts once WiFi is up. A task re-evaluates every contact
each second and right after any config change; the contact lights and the
buttons use the result. Not synced yet: every contact is available, logged
once, and the page shows "Clock not synced".

Time zones: each schedule stores a POSIX TZ rule (`EST5EDT,M3.2.0,M11.1.0`),
picked on the page from a built-in table of about 80 IANA zones (defaults to
the browser's own zone). newlib has one process-wide TZ, and its `setenv()`
leaks the old string whenever the new value is longer, so swapping TZ per
contact is not used. `src/tz_posix.c` evaluates the rule itself from UTC
with integer math (names, offsets, `Mm.w.d`, `Jn`, `n`, transition times up
to 167 h, southern hemisphere rules). It was checked on the host against
macOS tzdata 2026c for every zone in the table, every 30 minutes over three
years from 2026-10-10, with no mismatch. Note tzdata 2026c: British Columbia
and Alberta stay on summer time from November 2026, so the table lists them
as `MST7` and `CST6`.

A window is evaluated on a circular week of 10080 minutes, so 22:00 to 01:00
and Saturday into Sunday wrap naturally. "Next available" is the nearest
window start, shown in the contact's own time.

### Privacy

The phone talks to the parent's own Twilio account (`api.twilio.com`,
`serverless.twilio.com`, `serverless-upload.twilio.com`, the SIP domain and
Twilio media servers) and to NTP (`pool.ntp.org`, `time.google.com`) for the
clock. Nothing else.

Progress shows as plain-language steps on the page; failures show Twilio's
message plus a hint.

## Buttons

| State | Action | Result |
|---|---|---|
| idle | short press button i | call contact i (needs WiFi and Twilio setup) |
| idle | short press, slot empty or contact outside their hours | busy tone for 2.5 s, no call |
| calling / ringing / in call | press any button | hang up (CANCEL before answer, BYE after) |
| idle | hold button 1 (GPIO 4) 1.5 s | cycle test modes: auto -> A -> B -> C -> D -> auto |
| any | hold button 1 10 s | factory reset: erase the `kidphone` NVS namespace and the WiFi driver's copy, reboot into setup (LED blinks amber from 1.5 s on) |

The onboard RGB light is the phone's status (table above). Each contact light
is on while that person can be called, off when the slot is empty or they are
outside their hours, and blinks while they are being called.

What the earpiece plays:

- idle, no WiFi: slow quiet beep (480 Hz, 250 ms every 3 s), so a parent can
  tell "offline" from "dead"
- idle, WiFi up, Twilio not set up: US reorder, 480 + 620 Hz, 250 ms on/off
- idle, ready: US dial tone (mode C)
- after a failed call (4xx/5xx/6xx, no response, DNS, not set up), or a call
  Twilio answered and hung up within 5 s: US busy, 480 + 620 Hz, 500 ms
  on/off, for 5 s, then the idle sound again
- calling: nothing until the first provisional response
- ringing: local US ringback (440 + 480 Hz, 2 s on / 4 s off), unless early
  media arrives (a 183 with SDP), in which case the remote audio plays
  instead. Twilio sends a plain 180 with no media, so the ringback is local.
- in call: the remote party; your voice goes out through the mic
- after the call ends (either side): dial tone again

Placing a call resets any test mode, so the phone always comes back to dial tone.

### Test modes (long press, idle only)

| Mode | What it does | What you should hear |
|---|---|---|
| A | Wideband loopback: mic straight to earpiece | Your own voice, full bandwidth, small delay. |
| B | Telephone loopback: mic, low-pass, decimate to 8 kHz, G.711 mu-law encode and decode, interpolate back up | Your voice, narrow and "phone-like". This is the exact codec chain a call uses. |
| C | US dial tone, 350 Hz + 440 Hz | A steady, slightly beating hum. |
| D | Silence | Nothing (noise floor check). |

## Call flow

```
idle --press--> CALLING   DNS lookup of SIP_DOMAIN, INVITE (no auth)
                          <- 407 Proxy Authentication Required (or 401)
                          -> ACK, then INVITE again with Proxy-Authorization
                          <- 100 Trying
                RINGING   <- 180 Ringing or 183 Session Progress (SDP: start RTP, play ringback)
                IN_CALL   <- 200 OK (SDP) -> ACK, two-way audio
--press-->      ENDING    -> BYE (or CANCEL before the answer) <- 200 OK
                IDLE      dial tone again
```

Also handled: remote BYE (200 OK, back to idle), OPTIONS (200 OK), in-dialog
re-INVITE (200 OK with our SDP), 4xx/5xx/6xx finals (ACK, log, back to idle),
INVITE retransmission per Timer A (500 ms doubling, gives up after 32 s),
BYE/CANCEL retransmission (capped at 4 s, 32 s total), CANCEL deferred until
the first provisional response, a 200 that crosses our CANCEL (ACK then BYE),
digest auth with qop=auth or without qop, a 90 s no-answer timeout.

### NAT

The phone sits behind a home NAT and does not REGISTER. The first INVITE
carries the LAN address. Twilio's 407 echoes the top Via with `received=`
(public IP) and `rport=` (public port of the SIP socket); the retried INVITE
uses that public IP:port in Contact and the public IP in the SDP `c=` line.
The SDP `m=` line keeps the local RTP port (10000). RTP is symmetric: one
socket sends and receives, sending starts as soon as a 183 or 200 with SDP
arrives so the NAT pinhole opens, and Twilio latches onto the source of that
RTP. If media arrives from a different address than the SDP said, the phone
switches its sending to that address. During a call a CRLF keepalive goes to
the SIP server every 20 s so the remote BYE still gets through the NAT.

### Twilio side

- Account with an approved Primary Customer Profile (Trust Hub KYC), which is
  required before Twilio will sell a phone number. One US number as caller ID
  (about $1.15/month). Vietnam (or wherever the family is) must be enabled in
  Voice geographic permissions.
- A SIP Domain (`SIP_DOMAIN`) with a credential list containing
  `SIP_USER` / `SIP_PASSWORD`, used for **voice call authentication**
  (no registration needed).
- The domain's voice URL points at a Twilio Function, source in
  `twilio/voice.js`. It dials the E.164 user part of the SIP To URI, but only
  if it is in the `ALLOWED` environment variable (comma separated), so leaked
  SIP credentials cannot run up toll calls. `CALLER_ID` is the Twilio number.
- **Do not use `answerOnBridge`.** With it, Twilio sends early media (183)
  and then a 200 OK, and behind a home NAT all audio stopped at the answer.
  Without it the media stream only starts at the 200 OK and works in both
  directions. The phone plays its own ringback meanwhile.
- `TEST_MODE=echo` makes the function answer the phone itself: it speaks,
  records 8 s, and plays the recording back. That tests both audio directions
  with no second phone. Set it back to `off` afterwards (environment variable
  changes need a redeploy).
- The phone sends `INVITE sip:<contact number>@<SIP domain>`, so the user
  part is the E.164 number.
- All of the above except buying the number, KYC and geo permissions is now
  done by the phone itself (see Twilio provisioning).

## Reading the log

Good boot (abridged):

```
I main: kid_phone: button places a SIP call
I config: config: wifi set, twilio provisioned
I config: contact 0: ...1234, schedule off (always available)
I improv: Improv Serial ready on UART0
I setup: setup page: http://kidphone.local/ (port 80)
I wifi: connecting to "..." ...
I rtp: media socket on UDP port 10000
I sip: SIP UA ready: UDP port 5060 (account read from config at each call)
I usb_audio: ==== EARPIECE READY ====
I audio: engine: mic yes, earpiece yes, telephone path ready, wideband direct copy
I wifi: WIFI UP: ip 192.168.x.y, gw 192.168.x.1
I audio: audio mode NOWIFI -> C       (dial tone starts; SETUP instead if Twilio is not set up)
```

A call:

```
I main: BUTTON 0: call Grandma
I sip: calling ...1234 as kidphone-a1b2c3@<SIP_DOMAIN>
I sip: SIP server <SIP_DOMAIN> -> 54.x.x.x:5060, local 192.168.x.y:5060
I sip: CALL STATE IDLE -> CALLING (contact 0)
I sip: SIP >> INVITE sip:+1*******1234@<SIP_DOMAIN> SIP/2.0
I sip: SIP << SIP/2.0 407 Proxy Authentication Required
I sip: NAT: public address 1.2.3.4, SIP port 61234 (local 192.168.x.y:5060); used in Contact and SDP c=
I sip: SIP >> ACK ...
I sip: proxy challenge: realm "sip.twilio.com", qop auth
I sip: SIP >> INVITE ...              (Proxy-Authorization: <redacted>)
I sip: SIP << SIP/2.0 100 Trying
I sip: SIP << SIP/2.0 183 Session Progress
I sip: CALL STATE CALLING -> RINGING (183 session progress)
I sip: early media: remote media 168.86.x.x:1xxxx, PCMU
I rtp: first RTP packet out, ssrc ...
I rtp: first RTP packet in, ssrc ..., 160 byte payload
I sip: SIP << SIP/2.0 200 OK
I sip: dialog: remote target sip:..., 1 route(s)
I sip: CALL STATE RINGING -> IN_CALL (200 OK)
I rtp: rtp 5s: tx 250 pkts (err 0, audio overflow 0) | rx 250 pkts, lost 0, late 0, dup 0, other 0 | jb depth 3, underruns 0, overflow drops 0
I main: BUTTON 0: hang up (state IN_CALL)
I sip: CALL STATE IN_CALL -> ENDING (button)
I sip: SIP >> BYE ...
I sip: SIP << SIP/2.0 200 OK
I sip: CALL STATE ENDING -> IDLE (BYE accepted)
```

Every SIP message's first line is logged as `SIP >>` (sent) or `SIP <<`
(received). With `APP_SIP_LOG_FULL 1` in `app_config.h` (default 0) the full
message follows, indented; at 0 the full dumps are DEBUG only. Authorization header values are always redacted and the
destination number is masked. Retransmissions log one line with
`(retransmit)`.

RTP stats every 5 s during a call: `tx` should be about 250 (50 packets/s),
`rx` the same once media flows, `lost` / `late` near 0, `jb depth` around 3
(the prime level, 60 ms), `underruns` 0 in steady state.

Audio stats every 5 s (all states):

```
I audio: stats 5s: mode CALL | mic 48000Hz/2ch/16b earpiece 48000Hz/2ch/16b | in 240000 out 240000 frames | underruns 0 overruns 0 txfail 0 | mic peak 812 (-32.1 dBFS) | jitter buf 960
```

Failure signs:

- `wifi: connect failed (reason N), retrying`: wrong SSID/password or out of
  range; it retries every 2 s forever.
- `BUTTON n: no WiFi yet, cannot call`: pressed before `WIFI UP`.
- `BUTTON n: not set up yet`: Twilio provisioning has not run.
- `BUTTON n: ... outside their available hours`: schedule says no (busy tone).
- `schedule: clock not synced yet`: NTP not reached; schedules are ignored
  until it is.
- `DNS lookup of ... failed`: WiFi up but no DNS/internet.
- `no response to INVITE (Timer B)`: nothing came back in 32 s (UDP 5060
  blocked, wrong domain).
- `authentication failed` / `403 after digest auth`: the login was rejected
  (run setup again, which sets a fresh password), or another credential list
  on the domain holds the same username.
- `call result: ...`: the same plain-language text the page shows.

What the page says after a failed call (raw SIP status in small text):

| Where it fails | Page message |
|---|---|
| 403 to the INVITE that carried Proxy-Authorization, or 401/407 twice | Twilio rejected the phone's login. Run setup again. |
| 404 | Twilio did not find the phone's setup (SIP domain). Run setup again. |
| 480 / 486 / 600 / 603 | No answer or busy. |
| answered, then Twilio's BYE within 5 s | Twilio could not reach this number. Check Geo permissions for that country, and on a trial account only verified numbers can be called. |
| DNS, no WiFi, no response | The phone could not reach Twilio. Check the WiFi and the internet connection. |

Geo permission and trial-account blocks happen inside the Function's Dial,
after the SIP leg is answered, which is why they show up as a short call.
(A grandparent who declines within 5 s would read the same; rare enough.)
- RTP `rx 0 pkts` while IN_CALL: no media back (NAT/latching problem); check
  `NAT:` and `remote media` lines.
- Handset failures (`no handset connected`, `no usable format`, `stream start
  failed`) are as in stage 1; see the USB descriptor dump.

## Known issues and bring-up findings (2026-10-10)

- **DHCP (resolved):** on 2026-10-10 the bench Tenda router stopped answering
  DHCP DISCOVERs from the board entirely (lwIP DHCP debug showed DISCOVERs
  going out and no OFFER ever arriving, even with a different MAC). Power
  cycling the router fixed it with stock firmware settings. If a board
  associates but never gets an IP, restart the router first. The optional
  `WIFI_STATIC_IP` / `WIFI_STATIC_GW` override in `secrets.h` remains for
  debugging.
- The AB17X handset's configuration descriptor is larger than 256 bytes:
  `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=1024` (in `sdkconfig.defaults`).
- Its earpiece isochronous OUT endpoint has MPS 384, which none of the IDF
  FIFO bias presets allow. `usb_audio.c` sets a custom split of the 200-line
  FIFO: RX 88 lines (mic), periodic TX 96 (earpiece), non-periodic TX 16.
- In idle modes the mic is not drained, so the audio stats show about 500
  mic overruns per 5 s. Harmless.
- Bring-up logging is off by default: `APP_SIP_LOG_FULL 0`, and the USB host
  debug tags plus the HPRT port probe are behind `APP_USB_DEBUG` (default 0).

## Layout

```
src/main.c            app entry, button task (4 buttons: call / hang up; button 0 test modes / factory reset), effective audio mode
src/contact_leds.c/.h one light per contact (available / off / blinking while called)
src/schedule.c/.h     SNTP, per-contact availability evaluation
src/tz_posix.c/.h     POSIX TZ rule math (no global TZ, host-testable)
src/config_store.c/.h runtime config in NVS (WiFi, SIP account, contacts and schedules), secrets.h seeding
src/improv.c/.h       Improv Serial on UART0 (WiFi from the browser flasher)
src/setup_server.c/.h setup web page + JSON API, mDNS kidphone.local
src/setup_page.html   the setup page (embedded in flash)
src/twilio_provision.c/.h Twilio self-provisioning task
src/status_led.c/.h   onboard WS2812 status light
src/wifi.c/.h         WiFi station, auto reconnect, power save off, runtime credentials, scan
src/sip.c/.h          minimal SIP UA over UDP: INVITE + digest auth, CANCEL, BYE, NAT handling, call state
src/rtp.c/.h          PCMU RTP sender/receiver tasks, jitter buffer, stream buffer to the audio task
src/usb_audio.c/.h    USB host + UAC driver: hot-plug, descriptor log, format choice, volume/mute
src/audio_engine.c/.h audio task (core 1): mic jitter ring, modes A-D and CALL, stats
src/app_config.h      all tunables (button and LED GPIOs, block size, volumes, buffers, SIP/RTP ports and timers)
src/secrets.h         optional developer override (gitignored; template secrets.h.example)
twilio/voice.js       Twilio Function behind the SIP Domain voice URL (allowlist, echo test); embedded and uploaded by provisioning
web/                  browser flasher page + ESP Web Tools manifest (scripts/build_web_bin.sh builds the image)
components/g711/      G.711 mu-law encode/decode (reusable, pure functions)
components/resampler/ integer-ratio FIR decimator/interpolator (reusable)
```

## Web installer

`web/` is a static one-click installer page built on ESP Web Tools
(`esp-web-tools@10.2.1`, loaded from unpkg; no other third-party requests).
`scripts/build_web_bin.sh` merges bootloader (0x0), partition table (0x8000),
otadata (only if `partitions.csv` has one) and the app (offset read from
`partitions.csv`, currently 0x10000) into `web/firmware/kid_phone.bin`, with
flash mode/freq/size read from `sdkconfig.esp32s3` (dio, 80m, 16MB), and
writes the version into `web/manifest.json`.

```sh
scripts/build_web_bin.sh                    # merge the existing build
scripts/build_web_bin.sh --build            # pio run first, then merge
scripts/build_web_bin.sh --version 0.3.0    # set the manifest version
```

`web/firmware/*.bin` is gitignored on purpose: the image contains whatever
`src/secrets.h` held at build time (WiFi and SIP credentials). Only publish
an image built with empty secrets, where WiFi comes from Improv Serial and
Twilio from the phone's setup page. Deploy by uploading `web/` including the
`.bin`.

Local preview (Web Serial needs https or localhost):

```sh
python3 -m http.server -d web 8000
# then open http://localhost:8000 in Chrome or Edge
```
