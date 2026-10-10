# kid_phone, stage 2: the button places a real phone call

Toddler phone proof of concept. An ESP32-S3 is USB host to a LIXADA retro
telephone handset (USB Audio Class, "AB17X USB Audio", VID:PID 001f:0b23)
and a minimal SIP client over WiFi to a Twilio SIP Domain. Pick up the
handset, hear a dial tone, press the button, and the phone calls
`TEST_DEST_NUMBER` (G.711 mu-law, 8 kHz, RTP over UDP).

Stage 1 (audio path only) is done and verified on hardware: earpiece and mic
both run 48 kHz 2 ch 16-bit, and the old test modes A-D are still reachable
with a long press (see below).

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
- **Call button**: one lead to **GPIO 4**, the other lead to **GND**. No
  resistor needed (internal pull-up, software debounce of 30 ms). GPIO 4 is
  not a strapping pin, not USB, not PSRAM, not flash.

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

## Secrets

`src/secrets.h` (gitignored; copy `src/secrets.h.example` and fill it in) defines
`WIFI_SSID`, `WIFI_PASS`, `SIP_DOMAIN`, `SIP_USER`, `SIP_PASSWORD` and
`TEST_DEST_NUMBER` (E.164), plus optional `WIFI_STATIC_IP` / `WIFI_STATIC_GW`
(see Known issues). The SSID may contain non-ASCII (UTF-8) bytes; it is
passed to the WiFi driver as raw bytes. The password is never logged; the
destination number is logged masked to its last 4 digits.

## Button

| State | Action | Result |
|---|---|---|
| idle | short press (release before 1.5 s) | call `TEST_DEST_NUMBER` |
| calling / ringing / in call | press | hang up (CANCEL before answer, BYE after) |
| idle | hold 1.5 s | cycle test modes: auto -> A -> B -> C -> D -> auto |

What the earpiece plays:

- idle, before WiFi is up: silence (mode D)
- idle, WiFi up: US dial tone (mode C)
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
- The phone sends `INVITE sip:<TEST_DEST_NUMBER>@<SIP_DOMAIN>`, so the user
  part is the E.164 number.

## Reading the log

Good boot (abridged):

```
I main: kid_phone stage 2: button places a SIP call
I wifi: connecting to "..." ...
I rtp: media socket on UDP port 10000
I sip: SIP UA ready: UDP port 5060, server <SIP_DOMAIN>:5060, user handset1
I usb_audio: ==== EARPIECE READY ====
I audio: engine: mic yes, earpiece yes, telephone path ready, wideband direct copy
I wifi: WIFI UP: ip 192.168.x.y, gw 192.168.x.1
I audio: audio mode D -> C            (dial tone starts)
```

A call:

```
I main: BUTTON: place call
I sip: calling ...1234 as handset1@<SIP_DOMAIN>
I sip: SIP server <SIP_DOMAIN> -> 54.x.x.x:5060, local 192.168.x.y:5060
I sip: CALL STATE IDLE -> CALLING (button)
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
I main: BUTTON: hang up (state IN_CALL)
I sip: CALL STATE IN_CALL -> ENDING (button)
I sip: SIP >> BYE ...
I sip: SIP << SIP/2.0 200 OK
I sip: CALL STATE ENDING -> IDLE (BYE accepted)
```

Every SIP message's first line is logged as `SIP >>` (sent) or `SIP <<`
(received). With `APP_SIP_LOG_FULL 1` in `app_config.h` (default for
bring-up) the full message follows, indented; set it to 0 to drop the full
dumps to DEBUG. Authorization header values are always redacted and the
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
- `BUTTON: no WiFi yet, cannot call`: pressed before `WIFI UP`.
- `DNS lookup of ... failed`: WiFi up but no DNS/internet.
- `no response to INVITE (Timer B)`: nothing came back in 32 s (UDP 5060
  blocked, wrong domain).
- `authentication failed`: credential list mismatch, or the SIP Domain does
  not have the credential list enabled for calls.
- `call failed: 403/404/480/486/...`: Twilio or the far end refused; the
  status line says why (a 4xx right after auth usually means the voice URL
  or TwiML is wrong).
- RTP `rx 0 pkts` while IN_CALL: no media back (NAT/latching problem); check
  `NAT:` and `remote media` lines.
- Handset failures (`no handset connected`, `no usable format`, `stream start
  failed`) are as in stage 1; see the USB descriptor dump.

## Known issues and bring-up findings (2026-10-10)

- **DHCP never completes** on the bench router: the board associates, the
  router appears to hand out a lease (the Mac's ARP table showed the board's
  MAC at an address), but `IP_EVENT_STA_GOT_IP` never fires. Cause not found.
  Workaround: `WIFI_STATIC_IP` / `WIFI_STATIC_GW` in `secrets.h` skip DHCP
  (DNS is 8.8.8.8). This has to be fixed before anyone else can use the
  firmware.
- The AB17X handset's configuration descriptor is larger than 256 bytes:
  `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=1024` (in `sdkconfig.defaults`).
- Its earpiece isochronous OUT endpoint has MPS 384, which none of the IDF
  FIFO bias presets allow. `usb_audio.c` sets a custom split of the 200-line
  FIFO: RX 88 lines (mic), periodic TX 96 (earpiece), non-periodic TX 16.
- In idle modes the mic is not drained, so the audio stats show about 500
  mic overruns per 5 s. Harmless.
- Bring-up logging is still on: full SIP dumps (`APP_SIP_LOG_FULL`), USB host
  debug tags and the HPRT port probe in `usb_audio.c`.

## Layout

```
src/main.c            app entry, button task (call / hang up / test modes), effective audio mode
src/wifi.c/.h         WiFi station, NVS init, auto reconnect, power save off
src/sip.c/.h          minimal SIP UA over UDP: INVITE + digest auth, CANCEL, BYE, NAT handling, call state
src/rtp.c/.h          PCMU RTP sender/receiver tasks, jitter buffer, stream buffer to the audio task
src/usb_audio.c/.h    USB host + UAC driver: hot-plug, descriptor log, format choice, volume/mute
src/audio_engine.c/.h audio task (core 1): mic jitter ring, modes A-D and CALL, stats
src/app_config.h      all tunables (GPIO, block size, volumes, buffers, SIP/RTP ports and timers)
src/secrets.h         WiFi + SIP credentials, destination number (gitignored; template secrets.h.example)
twilio/voice.js       Twilio Function behind the SIP Domain voice URL (allowlist, echo test)
components/g711/      G.711 mu-law encode/decode (reusable, pure functions)
components/resampler/ integer-ratio FIR decimator/interpolator (reusable)
```
