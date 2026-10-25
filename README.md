# kid_phone: a toddler phone that calls family over Twilio

- Build guide: https://muonsortes.com/toddler-phone
- Browser installer: https://allanbinder.github.io/kid-phone/
- License: Apache 2.0 (see LICENSE)

Toddler phone. An ESP32-S3 is USB host to a LIXADA retro telephone handset
(USB Audio Class, "AB17X USB Audio", VID:PID 001f:0b23) and a minimal SIP
client over WiFi to a Twilio SIP Domain in the parent's own Twilio account.
Pick up the handset, press a button, and the phone calls
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
   (More networks can be added later from the setup page, see below.)
3. Press **Visit device** (or open `http://kidphone.local/`). On that page,
   fill in who each button calls (name and number, `+1 555 123 4567` style;
   one person is enough), paste the **Account SID** and **Auth Token** from
   the Twilio Console home page, press **Connect**. Setup takes about a minute.
4. Press **Call ...**, or plug in the handset, pick it up, and press a button.

Later changes: names, which button calls whom, available times and phone
numbers are saved with **Save** and need no token. The Auth Token is used
once during setup and forgotten; the phone keeps a limited Twilio key that
can only update the list of numbers it may call (the Twilio Function only
dials numbers on that list), and updates it in a few seconds when a number
changes. If that key stops working (deleted in the Twilio Console, say), the
page asks for the Auth Token once and makes a new key.

**Only when they're available** (per person, optional): pick their time zone
and the times they are happy to take calls (presets "Weekday evenings",
"Weekend mornings", "Every day, daytime", or custom days and times, up to
four; a time like 22:00 to 01:00 runs past midnight). Outside those times
that person's light is off and their button says "They can't talk right
now" (then a short busy tone) instead of calling. Until the phone's clock has synced from the internet, everyone
counts as available (a clock problem never blocks a call).

**Call length**: calls end by themselves after 30 minutes (change it on the
setup page, 1 to 120 minutes). The phone hangs up and says "Time's up!";
pressing the button again calls back. After any call ends, presses are
ignored for 3 seconds, and while the phone is talking, so a child mashing
the buttons cannot place a burst of calls.

**Bedtime** (optional, off by default): on the setup page's **Bedtime**
card, turn on "Put the phone to sleep at night" and set when the lights go
off and come back on (say 19:30 to 07:00; a window that ends earlier than
it starts runs into the next morning). Pick the **home time zone**, where
the phone is (it starts on your computer's own zone). During bedtime every
contact light is off, even for people who are available, and pressing a
button says "It's sleep time. You can call in the morning." (then a short
busy tone) instead of calling. At the end time the lights come back on by
themselves, each following that person's own times as usual. A call that
is already going when bedtime starts is not cut off (the call length limit
still ends it), and the **Call** buttons on the setup page keep working, so
you can still test. Until the phone's clock has synced, bedtime is not
applied. The card shows what is happening now, for example "Asleep now,
lights back on at 07:00" or "Lights go off at 19:30". Changing bedtime
never needs the Auth Token.

If a person is in another country, turn that country on in the Twilio
Console under Voice > Settings > Geo permissions. The phone does not change
that setting.

**More than one WiFi network**: the phone remembers up to 5 WiFi networks
(for example home, grandma's house, a phone hotspot). On the setup page,
the **WiFi networks** card lists them and shows which one the phone is on
and how strong its signal is. **Scan for networks** lists what the phone can
see nearby (name, signal bars, secured or open; the phone uses 2.4 GHz WiFi
only); tap one, or type a name for a hidden network, enter the password
(8 to 63 characters, empty for an open network) and press **Add**. Adding a
network does not move the phone off the one it is on: the new one is used
the next time the phone connects. Adding a name that is already saved
changes its password. **Remove** forgets a network; removing the one the
phone is on right now asks you to confirm (the phone then switches to the
strongest other saved network), and the last saved network cannot be
removed (the phone could never get back on WiFi; use a factory reset to
start over). Passwords are never shown again, not even on the setup page.

At power-on and whenever the WiFi drops, the phone looks at all the
networks around it and joins the strongest saved one it can see (with mesh
WiFi or extenders, the strongest access point of that network). If that
fails (wrong password, say) it tries the next one. If none works it tries
again after 10 seconds, then 20, 40 and every 60 seconds, forever. Once it
is on a network that works it stays there, even if another saved network
is stronger: it only switches after a disconnect.

**Why is it on that network?** The phone keeps a short connection history
in memory (cleared when it restarts), so no computer cable is needed. Under
each saved network the WiFi card shows one line: the last try if it failed
("Last try: wrong password (5 min ago)") or how strong the last scan saw it
("Last seen -66 dBm"). A wrong password is shown in orange with what to do:
press **Remove**, then add the network again with the right password
(passwords are case sensitive). **Recent connection attempts** (collapsed,
under the list) shows the last 8 attempts, newest first: network, result
(connected, wrong password, network not found, no answer from the router,
joined but no IP address from the router, connection dropped, or the
driver's reason in words), the signal the phone saw just before, and how
long ago. The **WiFi settings** link next to the WiFi line at the top of
the page jumps to this card.

**Taking the phone to another house**: add that house's WiFi on the setup
page **before** you leave, while the phone is still on your WiFi (you need
its name and password). At the other house the phone joins it by itself.
If you forgot: plug the phone's COM port into a computer there, open the
installer page, press **Connect**, and use the WiFi setup (Improv) to add the
network; the networks already saved are kept. As a last resort, hold BOOT
for 10 s (factory reset) and set the phone up from step 1.

**Starting over (factory reset)**: on the setup page open **Advanced**, type
`RESET` and press **Factory reset**. Or hold the **BOOT** button on the
phone's board for 10 seconds (the light blinks amber from 1.5 s on; let go
before 10 s to cancel). Either way the phone forgets everything (all saved
WiFi networks, Twilio connection, people, times, call length, bedtime) and restarts ready for step 1
above. Nothing in your Twilio account is deleted. The contact buttons cannot
reset the phone: holding them does nothing.

The earpiece is **silent while the phone is idle**, whatever its state:
there is no hook switch yet, so a handset lying on the table would otherwise
play a tone all day. The lights are the "on" indicator. The phone only makes
a sound in answer to a press: the call itself, a spoken sentence explaining
why it cannot call, and a short busy tone after a failed call or a refused
press. Then it is silent again. (`APP_IDLE_DIAL_TONE 1` in
`src/app_config.h` brings back the old idle tones: dial tone when ready, a
slow beep with no WiFi, fast busy when not set up.)

What the lights and the earpiece tell you:

| Contact lights | Board light | Earpiece | Meaning |
|---|---|---|---|
| all off | red blinking | silent | no WiFi (a press says "The phone isn't connected") |
| all off | blue | silent | WiFi OK, Twilio not set up yet (a press says "This phone isn't ready yet") |
| on for whoever can be called | green | silent | ready, press a lit button to call |
| off for one person | green | silent | that slot is empty, or the person is outside their available hours |
| all off | green | silent | bedtime (a press says "It's sleep time") |
| | blue pulsing | silent | Twilio setup running |
| that person's light blinks | white pulsing | ringback, then the call | in a call |
| | | spoken sentence, then busy (480 + 620 Hz, 500 ms on/off) for 5 s | the last call failed |
| | | spoken sentence, then busy for 2.5 s | bedtime, the button pressed has nobody set up, or the person is outside their available hours |
| | amber fast blink | | BOOT button held: keep holding for 10 s to erase everything, let go to cancel |

No lights at all means the phone is not working (no WiFi, not set up, no
power) or nobody can be called right now; pressing a button then explains
which with a spoken sentence.

What the phone says (it speaks instead of only beeping, then goes silent
again):

| What the phone says | What it means | What to do |
|---|---|---|
| "Time's up! Press the button again to call again." | The call reached the call length limit and the phone hung up. | Nothing. Raise the limit on the setup page if calls should last longer. |
| "They can't talk right now. Try again later." | That person is outside the available times set for them. | Nothing, or change their times on the setup page. |
| "They didn't answer the phone." | The person did not pick up within 30 s, or declined the call (Twilio says it, then about 4 s of quiet, then hangs up). The phone says the same words itself if Twilio rejects the call as busy, declined or not answered (SIP 480, 486, 600, 603), or it rang for 90 s with no answer. | Nothing. |
| "The phone isn't connected. Ask a grown-up for help." | No WiFi, or WiFi without internet (the phone could not look up or reach Twilio), or the WiFi dropped during a call. | Check the router and internet. A red blinking board light means no WiFi at all: none of the saved networks is in range or accepts the saved password (a new house, a new router, a changed password). Add the network from the setup page, or from the installer page (see "Taking the phone to another house"). |
| "It's sleep time. You can call in the morning." | Bedtime is on and it is inside the bedtime hours (home time). | Nothing, or change bedtime on the setup page. |
| "This phone isn't ready yet. Ask a grown-up for help." | Twilio setup has not been done, or the button pressed has nobody set up. | Open `http://kidphone.local/`, finish setup or fill in that button. |
| "Oops, that call didn't work. Let's get a grown-up to help." | Twilio refused the call: login rejected (401/403/407, run setup again), SIP domain gone (404), the call was cut off by Twilio within 5 s (usually Geo permissions for that country, or a trial account calling an unverified number), or any other error (credit, account suspended). | Open the setup page: **Try it** shows the detailed reason of the last call. Then fix it in the Twilio Console or press **Run setup again**. |

Note on calls that ring out: Twilio answers the phone's call at once and
plays the ringing itself (see Twilio side below), so the phone cannot tell
an unanswered call from a normal one. The Twilio Function therefore says
"They didn't answer the phone." on the call itself, in the same voice as
the phone, before it hangs up. Its quiet tail keeps that call longer than
5 s, so the phone does not take it for a Twilio block. "Sorry, this phone
can't call that number." (Twilio's own voice) only plays if the number is
not on Twilio's list for this phone, which means the phone and Twilio are
out of step: press **Save** on the setup page, or **Run setup again**.

After a firmware update the Status box may say the Twilio account still
has the call program from older phone software. Press **Run setup again**
once (it needs the Auth Token); changing numbers alone does not update it.

Contact lights: on = can be called now, off = not set up, outside their
hours, bedtime (all off), or the phone itself cannot call (no WiFi or Twilio
not set up; then all are off), blinking = being called.

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
  | 1 | GPIO 4 | GPIO 15 |
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
- **BOOT button** (on the board, GPIO 0, active low, on-board pull-up): the
  parent-only button for test modes and factory reset (see Buttons). GPIO 0
  is a strapping pin, so the firmware only reads it after boot and never
  drives it. Holding it while the board resets still starts the ROM
  downloader, as for flashing.

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
| `w0_ssid`/`w0_pass` .. `w4_ssid`/`w4_pass` | str | up to 5 saved WiFi networks, oldest first, packed from slot 0 (empty password = open network). Improv Serial (only after the connection works) and `POST /api/wifi/add` / `remove` |
| `wifi_ssid`, `wifi_pass` | str | legacy (older firmware kept its one network here). At boot it is moved into the list once (into the first free slot; with a full list the oldest is dropped), committed, and only then the old keys are erased, so a power cut never loses it |
| `sip_domain`, `sip_user`, `sip_pass` | str | Twilio provisioning |
| `caller_id` | str | provisioning (the Twilio number, E.164) |
| `tw_sid` | str | provisioning (Account SID, not a secret; prefills the page and enables the numbers-only update) |
| `c0_name`/`c0_num` .. `c3_name`/`c3_num` | str | provisioning or `POST /api/contacts`; slot i = button i, empty number = unused |
| `c0_sch` .. `c3_sch` | blob, 111 bytes | same; availability schedule, missing = always available |
| `provisioned` | u8 | provisioning (1 = done; trusted only if the SIP account and at least one number exist) |
| `tw_key_sid`, `tw_key_sec` | str | provisioning; restricted Twilio API key (`SK...`) and its secret, used only for number updates. A sid without a secret = a key Twilio rejected, deleted on the next token run |
| `tw_svc`, `tw_env` | str | provisioning; Serverless service and environment SIDs the key updates |
| `max_call_min` | u16 | `POST /api/settings`; longest call in minutes, 1..120, missing or out of range = 30 |
| `bed_on` | u8 | `POST /api/bedtime`; 1 = bedtime on, missing = off (configs from before 0.4.0 load unchanged) |
| `bed_start`, `bed_end` | u16 | same; bedtime window in minutes of the day, home time, missing or out of range = 1170 (19:30) / 420 (07:00) |
| `home_tz`, `home_tz_name` | str | same; home time zone (POSIX TZ rule) and the IANA name the page picked it from (display only), missing = not set yet |

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

What the phone does keep is a restricted Twilio API key ("numbers" key,
friendly name `kidphone-<suffix> numbers`, the same suffix as the SIP login),
created through `iam.twilio.com` with exactly this policy:

```json
{"allow":["/twilio/serverless/environments/read","/twilio/serverless/variables/list","/twilio/serverless/variables/create","/twilio/serverless/variables/update","/twilio/serverless/deployments/create"]}
```

It can read the environment, set its variables and deploy, which is what a
number change needs, and nothing else (checked by hand 2026-10-18: calls,
account, services and functions all answer 401). Worst case for a leaked key:
someone can change `ALLOWED` / `CALLER_ID` of the call program. The secret is
never logged and never returned by any endpoint; `/api/status` only says
whether a key exists. A factory reset erases it (the key itself stays in the
Twilio account until the next setup on that account deletes it, or the
parent deletes it under Console > Account > API keys).

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
WiFi settings (state Provisioning; the phone leaves whatever network it is on
and tries this one for up to 20 s; on success it is **added** to the saved
networks, or its password updated if that SSID is already saved; the others
stay. With 5 already saved the oldest one makes room. Reply Provisioned with
`http://kidphone.local/` and `http://<ip>/`. On failure: error "unable to
connect", nothing is saved, and the phone goes back to its saved networks),
current state, device info (`kid_phone`, app version, `ESP32-S3`, `Kid
Phone`), WiFi scan (an empty list during a call). Others get "unknown RPC
command".

### WiFi connection policy

`src/wifi.c` runs one manager task that owns every connect, disconnect and
scan, so driver events and requests never race:

- At boot and after every disconnect: an all-channel active scan, then the
  saved networks that were seen are tried strongest RSSI first. Each attempt
  keeps `WIFI_ALL_CHANNEL_SCAN` + `WIFI_CONNECT_AP_BY_SIGNAL`, so it joins
  the strongest access point of that SSID. If the scan saw a hidden network,
  failed, or came back empty, saved networks it did not see are tried after
  those (one may be the hidden one).
- An attempt fails on a disconnect event (wrong password, not found) or
  after `APP_WIFI_ATTEMPT_MS` (20 s, covers association, handshake and
  DHCP); the next candidate follows. When none works: wait
  `APP_WIFI_BACKOFF_MIN_MS` (10 s), doubling to `APP_WIFI_BACKOFF_MAX_MS`
  (60 s), then rescan. Never gives up; the pause resets to 10 s after a
  successful connect, and saving a network while offline starts a new round
  at once.
- Connection history (RAM only, reset at boot): every attempt end, and every
  drop of an established connection, goes into a ring of 8
  (`wifi_attempts()`), with the RSSI that network had in the scan before it.
  Every finished scan (the round's or the setup page's) also updates each
  saved network's last RSSI, and every attempt its last result
  (`wifi_net_info()`); re-saving a network from the page clears its last
  result. Disconnect reasons map to results: 15, 202, 204 = wrong password;
  201, 210 to 212 = not found; the attempt timer = timed out, or no IP when
  the access point had associated (DHCP never answered).
- A disconnect event whose SSID is not the network being tried (or on) is a
  late event from an earlier attempt and is ignored, so it cannot cut short
  the next attempt.
- No roaming: a working connection is kept until it drops. The one
  exception is removing the connected network on the setup page: the phone
  leaves it about 1.5 s after replying and runs a new round.
- The driver's own NVS copy of the config is off (`WIFI_STORAGE_RAM`), so
  switching networks writes no flash.
- Everything that depends on WiFi reads `wifi_is_connected()`, which is
  only true with an IP: status LED, contact LEDs, the "isn't connected"
  prompt, SNTP (started once, keeps running across networks), mDNS (the
  component re-announces `kidphone.local` on every new IP). SIP reads the
  local IP at the start of each call and both sockets are bound to
  `INADDR_ANY`, so the next call after a switch uses the new address and
  learns the new NAT mapping. A call in progress when the WiFi is gone for
  `APP_SIP_NET_LOSS_MS` (10 s), or when the phone shows up with a different
  local IP, is ended locally (result kind `network`, the phone says "The
  phone isn't connected"), since nothing can reach its old addresses.

### Setup web page

`esp_http_server` on port 80 plus mDNS `kidphone.local` (`src/setup_server.c`,
page in `src/setup_page.html`, embedded in flash, no external assets). The
phone only has the station interface, so it is only reachable on the home LAN.

| Endpoint | What |
|---|---|
| `GET /` | setup page |
| `GET /api/status` | `settings` {`max_call_minutes` and its bounds}, `bedtime` {`enabled`, `start`, `end` ("HH:MM", home time), `tz`, `tz_name`, `scheduled` = on with a valid home time zone, `active` = asleep now}, WiFi, Twilio (provisioned, caller ID, account SID, SIP user, `login_current`, `numbers_key` = a restricted key is stored), 4 contact slots with schedule, `available`, `next` {day, time} in their time zone, `time_synced`, handset, call state and contact, last call result (`code`, `kind`, `message`), setup progress (`state`, `run`, `steps`, on failure `error`, `hint`, `needs_token`) |
| `POST /api/contacts` | `{contacts:[{name, number, schedule:{enabled, tz, tz_name, windows:[{days, start, end}]}} x up to 4]}`; saves names, button order and schedules without a token. If the set of numbers differs from what Twilio allows and a numbers key is stored: 202 `{updating: true, run}`, the key run starts (progress in `/api/status`; if Twilio rejects the key it fails with `needs_token`). No key: 409 `{needs_token: true}` |
| `POST /api/provision` | `{account_sid, auth_token, contacts, full}` (202 `{ok, message, run}`). Already set up with its own login, same account, `full` not true: numbers-only update. Otherwise the full setup. `account_sid` may be omitted once stored. Old `contact_name`/`contact_number` still accepted |
| `POST /api/settings` | `{max_call_minutes}`, a whole number 1..120 (400 otherwise); no token |
| `POST /api/bedtime` | `{enabled, start, end, tz, tz_name}`: times as "HH:MM" or minutes of the day (home time), `tz` a POSIX TZ rule. 400 if a time is missing or out of range, if on with equal times, or if `tz` does not parse (required when on). Device only, no token |
| `POST /api/call` | `{contact: i}` test call, same rules as button i (409 if outside their hours) except bedtime, which does not apply to it (the parent testing) |
| `POST /api/hangup` | hang up |
| `GET /api/wifi` | `{connected, state` ("connected", "connecting", "scanning", "waiting", "no networks")`, ssid, rssi, ip, max, uptime_s, saved: [{ssid, secured, connected, in_last_scan, last_rssi, last_seen_ago_s, last_result, last_text, last_reason, last_try_ago_s}], attempts: [{ssid, rssi, result, text, reason, at_s, ago_s, improv}]}`. `attempts` = the last 8, newest first (RAM only, since boot). `result`/`last_result` is one of `connected`, `wrong_password`, `not_found`, `timeout`, `no_ip`, `dropped`, `other`; `text` is the plain-English version shown on the page; `reason` is the driver's disconnect reason (0 = none). History fields are left out when unknown: `in_last_scan` before the first scan, `last_rssi`/`last_seen_ago_s` if no scan saw it, `last_*` result fields if it was never tried, `rssi` if the scan before that attempt did not see it, `improv` unless true. The same object is `wifi` in `/api/status`. Never a password |
| `POST /api/wifi/scan` | `{}`. Blocks a few seconds; `{networks: [{ssid, rssi, bars (1-4), secured, saved, connected}]}`, one entry per SSID (strongest access point), strongest first, hidden networks left out. The connection stays up. 409 during a call (the scan would pause the audio), 503 if the radio is busy |
| `POST /api/wifi/add` | `{ssid, password}`: SSID 1..32 bytes (not trimmed), password empty (open) or 8..63 characters (400 otherwise). Same SSID = new password (`updated: true`). 409 when 5 are saved and this one is new. The current connection is kept |
| `POST /api/wifi/remove` | `{ssid, confirm}`: 404 if not saved; 409 if it is the only saved network; 409 `{needs_confirm: true}` if the phone is on it and `confirm` is not true. With confirm the phone leaves it after the reply and joins the best other saved network |
| `POST /api/factory_reset` | `{confirm: "RESET"}` (exactly, checked on the phone; 400 otherwise, 409 while setup runs). Replies 202, then about 0.5 s later stops any call, erases the `kidphone` NVS namespace and the WiFi driver's copy, and reboots into setup |

POST bodies must be `application/json` (a cross-site page cannot send that
without a CORS preflight, which is never answered), and the Host header must
be `kidphone.local` or an IP literal (DNS rebinding). No other auth in v1:
anyone on the LAN can rerun setup, place a test call or factory reset the
phone.

### Twilio provisioning

`src/twilio_provision.c`, its own task (16 KB stack, TLS buffers in PSRAM),
HTTPS with the ESP-IDF certificate bundle, Basic auth SID:token (key runs:
key sid:secret). Only `api.twilio.com`, `serverless.twilio.com`,
`serverless-upload.twilio.com` and `iam.twilio.com` are contacted. Every step reuses what an earlier run created, so it is safe to
rerun ("Run setup again" on the page reruns it).

Retries: a request that fails in transport ("could not reach Twilio") or
gets 429 or 5xx is tried up to 4 times in all, 1 s, 2 s and 4 s apart
(`APP_PROV_HTTP_TRIES`, `APP_PROV_RETRY_MS`). GETs, DELETEs (a 404 on a
repeat counts as done), updates and creates whose duplicate is harmless
(function or asset version, build, deployment) retry freely. Creates that must exist
once (credential list, credential, SIP domain, credential mapping, service,
environment, function, asset, variable, numbers key) are retried on their own only
when the request provably never reached Twilio (the connection failed before
the whole request went out) or got 429. When a create may have reached
Twilio without an answer, the phone looks it up by name before trying again
and uses it if it is there; for the numbers key, whose secret only comes in
the create answer, it deletes any key with the phone's key name instead.
The SIP login is saved the moment Twilio accepts the new password, and the
contacts the moment the deployment with the new `ALLOWED` is live, so a run
that stops later cannot leave the phone out of step with Twilio.

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
5. Serverless service `kid-phone`, environment `prod`, function `voice`,
   asset `no-answer` (each looked up by name before it is created); upload
   `twilio/voice.js` (embedded at build time, one source of truth) as
   `/voice`, protected, and `twilio/no_answer.wav` as `/no-answer.wav`,
   public (the spoken "They didn't answer the phone." for the Function's
   `<Play>`; only that sentence, nothing about the family); one build with
   both versions and `APP_PROV_FN_DEPS`; poll until completed (2 min
   timeout); set `CALLER_ID` and `ALLOWED`; deploy; save
   `APP_TW_PROGRAM_VERSION` (NVS `tw_prog`). A phone whose saved value is
   lower than the firmware's (0 = set up by firmware before this marker)
   shows "Run setup again" in the Status box. Bump the constant whenever
   `twilio/voice.js` or an asset changes. A rerun uploads a new function and
   asset version (the old ones stay unused, like before).
6. SIP domain `VoiceUrl = https://<env domain>/voice` (POST).
7. Save account SID, SIP domain/user/password, caller ID, contacts with
   schedules, `provisioned=1`.
8. Numbers key: `DELETE iam.twilio.com/v1/Keys/{old sid}` if one is stored
   (failure ignored), then `POST iam.twilio.com/v1/Keys` (`KeyType=restricted`,
   the policy above); save key sid, secret, service and environment SIDs. A
   failure is a "Warning:" step, not a failed setup, and leaves no key, so
   number changes ask for the Auth Token as before.

Numbers-only update with the token (numbers changed on a phone already set
up with its own login, no usable key): 1, find the `kid-phone` service and
`prod` environment, set `ALLOWED`, redeploy the environment's current
`build_sid`, save the contacts, then create the numbers key if none usable
is stored (phones set up by older firmware get one here). If anything is
missing it falls back to the full run. **Run setup again** on the page
always does the full run (fresh password, fresh key).

Numbers-only update with the key (the usual case, started by **Save**): no
account check (the key cannot read the account), `GET` the stored
environment for its `build_sid`, set `ALLOWED`, deploy, save the contacts.
On 401 / 403 / 404 (key deleted, service or environment gone) the run fails
with `needs_token`, the stored secret is dropped, and the page asks for the
Auth Token and reruns the update the token way.

Every failed step adds a `Failed: <step> (HTTP <status>)` line to the
progress log (and a `key run failed at "<step>": HTTP ...` line on the
serial log). The page keeps the edits until the run is confirmed: it says
"Saved. Button 2 (Name) now calls ...1234." only after `/api/status` shows
the run done and the new numbers stored; any other outcome shows the error
under **Save** with **Try again** and **Use the Auth Token instead**. The
status card's **Changing numbers** row shows whether a numbers key is stored.

Both numbers-only updates redeploy the build that is live. A Twilio build
holds its function and asset versions, so the no-answer clip stays deployed
and the key needs no asset permissions. It also means they never install a
newer `voice.js`: only the full run does (hence the "Run setup again" note).

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

### Bedtime

`schedule_bedtime_eval()` in `src/schedule.c`: one window every day, in the
home time zone (`home_tz`, the same POSIX TZ evaluation as the schedules),
on a circular day of 1440 minutes, so 19:30 to 07:00 wraps past midnight.
The schedule task caches it each second (`schedule_bedtime_active()`) and
logs each change. While it is active the contact lights are all off
(`contact_leds.c`; a call already going keeps blinking and is never hung up
by bedtime) and an idle press plays `sleep_time` and the short busy tone
(`on_press_idle()` in `main.c`, checked before the WiFi, setup and schedule
checks). Not synced yet, off, or no valid home time zone: never active, so a
clock problem never blocks a call. `POST /api/call` skips it on purpose.

### Privacy

The phone talks to the parent's own Twilio account (`api.twilio.com`,
`serverless.twilio.com`, `serverless-upload.twilio.com`, `iam.twilio.com`, the SIP domain and
Twilio media servers) and to NTP (`pool.ntp.org`, `time.google.com`) for the
clock. Nothing else.

Progress shows as plain-language steps on the page; failures show Twilio's
message plus a hint.

## Buttons

| State | Action | Result |
|---|---|---|
| idle | short press button i | call contact i (needs WiFi and Twilio setup) |
| idle | short press, bedtime, slot empty or contact outside their hours | spoken prompt, then busy tone for 2.5 s, no call |
| idle | short press while a prompt plays, or within 3 s (`APP_PRESS_COOLDOWN_MS`) after a call ended or failed | ignored (button mashing) |
| calling / ringing / in call | press any button | hang up (CANCEL before answer, BYE after); the 3 s cooldown then starts |
| in call | call reaches the call length limit (setup page, default 30 min) | BYE, then "Time's up!", then silence |

A contact button acts once, on the press edge (it does not wait for the
release). Holding it does nothing more and never repeats: the next action
needs a release and a new press. Contact buttons cannot change test modes or
reset the phone; those are on the board's BOOT button, out of a child's
reach:

| State | BOOT button (GPIO 0) | Result |
|---|---|---|
| idle | short press (released before 1.5 s) | cycle test modes: auto -> A -> B -> C -> D -> auto |
| any | hold 1.5 s to 10 s, then let go | nothing (the status LED blinks amber from 1.5 s on as a warning) |
| any | hold 10 s | factory reset: stop any call, erase the `kidphone` NVS namespace and the WiFi driver's copy, then reboot into setup once BOOT is released (or after 30 s if it stays held), so the strapping pin is high at the reset |

The setup page can do the same factory reset (`POST /api/factory_reset`,
**Advanced** on the page, type `RESET`).

The onboard RGB light is the phone's status (table above). Each contact light
is on while that person can be called, off when the slot is empty or they are
outside their hours, and blinks while they are being called. When the phone
cannot call anyone (no WiFi, or Twilio not set up) every contact light is off.

What the earpiece plays:

- idle, any state (ready, no WiFi, not set up): silence (exact digital
  zeros). With `APP_IDLE_DIAL_TONE 1` instead: no WiFi = slow quiet beep
  (480 Hz, 250 ms every 3 s), Twilio not set up = US reorder (480 + 620 Hz,
  250 ms on/off), ready = US dial tone (mode C)
- a press that cannot call: the spoken prompt for it, then (bedtime, empty
  slot or outside their hours) US busy for 2.5 s, then silence
- after a failed call (4xx/5xx/6xx, no response, DNS, not set up), or a call
  Twilio answered and hung up within 5 s: the spoken prompt for it (table in
  Setup), then US busy, 480 + 620 Hz, 500 ms on/off, for 5 s, then silence
- after the call length limit or a 90 s ring timeout: the spoken prompt, then
  silence
- calling: nothing until the first provisional response
- ringing: local US ringback (440 + 480 Hz, 2 s on / 4 s off), unless early
  media arrives (a 183 with SDP), in which case the remote audio plays
  instead. Twilio sends a plain 180 with no media, so the ringback is local.
- in call: the remote party; your voice goes out through the mic
- after the call ends (either side): silence

Tones start at phase zero and every start, stop and cadence edge ramps over
5 ms (`APP_TONE_RAMP_MS`); switching away from a tone plays one more 10 ms
block fading to zero first. The prompt clips carry their own 5 ms fades. So
going between silence, prompts and the busy tone does not click.

Placing a call resets any test mode, so the phone always comes back to its
normal idle sound (silence).

### Test modes (BOOT short press, idle only)

Bench checks for the handset audio path (USB audio, the codec chain). The
normal idle sound does not change: without a BOOT press the phone is silent
at idle as above. Mode C is the only way to hear the dial tone by default.


| Mode | What it does | What you should hear |
|---|---|---|
| A | Wideband loopback: mic straight to earpiece | Your own voice, full bandwidth, small delay. |
| B | Telephone loopback: mic, low-pass, decimate to 8 kHz, G.711 mu-law encode and decode, interpolate back up | Your voice, narrow and "phone-like". This is the exact codec chain a call uses. |
| C | US dial tone, 350 Hz + 440 Hz | A steady, slightly beating hum. |
| D | Silence | Nothing (noise floor check). |

### Spoken prompts

The phone speaks seven short sentences (table in Setup). They are made
offline by `scripts/gen_prompts.py` and committed as raw 16 kHz mono 16-bit
little-endian PCM in `src/prompts/` (about 850 KB in all, embedded with
`EMBED_FILES` in `src/CMakeLists.txt` and `board_build.embed_files` in
`platformio.ini`; the two lists must match). The phone never synthesizes
speech and never fetches audio. The audio task plays a prompt through its own
FIR interpolator (16 kHz to the earpiece rate, 3x for the 48 kHz handset,
copied to both channels), one 10 ms block per pass like every other mode, so
it cannot stall or underrun the earpiece. A prompt starting a call is
dropped; a handset whose rate is not a multiple of 16 kHz gets tones only.

Voice: [Piper](https://github.com/rhasspy/piper) `en_US-ljspeech-high`
(rhasspy/piper-voices, pinned revision and sha256 in the script). Its
MODEL_CARD lists the training data as the LJ Speech dataset, license public
domain, and the voice was trained from scratch on it, so the generated clips
can be redistributed with this repo. Do not switch to a voice fine-tuned
from `lessac` (Blizzard license) or trained on CC BY-NC data without
checking its MODEL_CARD first.

Levels: active speech at about -23 dBFS RMS with peaks held at or below
-7 dBFS, a little under the dial tone (-20 dBFS RMS, -14 dBFS peak) and
about level with the status tones. The earpiece runs at 100 % volume against
a child's ear, so keep it there. Speech is slowed a little (Piper length
scale 1.2) and high-passed at 120 Hz for the small earpiece.

To change a sentence, edit `PROMPTS` in the script and run it with only the
clips that changed (Piper output varies slightly between runs, so the others
would change too; listen to the result):

```
python3.12 -m venv ~/.venv-piper && ~/.venv-piper/bin/pip install piper-tts
~/.venv-piper/bin/python scripts/gen_prompts.py no_answer   # needs ffmpeg on PATH; no names = all
```

`no_answer` also writes `twilio/no_answer.wav`, the same mastered clip as
8 kHz mono mu-law WAV (the call's codec, so Twilio does not transcode it),
which Twilio plays on the call when nobody picks up. Call audio reaches the
earpiece with no extra gain, so it is as loud as the phone's own copy.
Changing it (or `twilio/voice.js`) needs a bump of `APP_TW_PROGRAM_VERSION`
so set-up phones ask for **Run setup again**.

## Call flow

```
idle --press--> CALLING   DNS lookup of SIP_DOMAIN, INVITE (no auth)
                          <- 407 Proxy Authentication Required (or 401)
                          -> ACK, then INVITE again with Proxy-Authorization
                          <- 100 Trying
                RINGING   <- 180 Ringing or 183 Session Progress (SDP: start RTP, play ringback)
                IN_CALL   <- 200 OK (SDP) -> ACK, two-way audio
--press-->      ENDING    -> BYE (or CANCEL before the answer) <- 200 OK
                IDLE      silent again
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
  SIP credentials cannot run up toll calls (otherwise it says "Sorry, this
  phone can't call that number." and hangs up). `CALLER_ID` is the Twilio
  number. The `<Dial>` (30 s timeout) has the Function itself as its
  `action`, so Twilio asks it again when the dial ends, with
  `DialCallStatus`, while the phone's leg is still up:
  - `no-answer` or `busy` (declined): `<Play>` the public asset
    `https://<env domain>/no-answer.wav` ("They didn't answer the phone.",
    the phone's own Piper voice, 8 kHz mu-law WAV made by
    `scripts/gen_prompts.py`), `<Pause length="4">`, `<Hangup>`. The pause
    keeps the call past `APP_SHORT_CALL_MS` (5 s) so the phone does not add
    its "can't make calls" sentence; change both together. `canceled` (the
    phone hung up while ringing) gets the same answer, which Twilio ignores.
  - `failed` (Geo permissions, trial account, bad number): `<Hangup>` at
    once. The phone sees a call cut off within 5 s and says "The phone can't
    make calls right now. Ask a grown-up to check the phone."
  - `completed`: `<Hangup>`.
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
I wifi: scan: 9 access points, 1 of 2 saved networks in range ("Home" -48 dBm)
I wifi: connecting to "Home" ...
I rtp: media socket on UDP port 10000
I sip: SIP UA ready: UDP port 5060 (account read from config at each call)
I usb_audio: ==== EARPIECE READY ====
I audio: engine: mic yes, earpiece yes, telephone path ready, wideband direct copy
I wifi: WIFI UP: ip 192.168.x.y, gw 192.168.x.1
I wifi: connected to "Home"
(no audio mode change: idle stays D, silence; with APP_IDLE_DIAL_TONE 1 it
 goes NOWIFI -> C, or SETUP if Twilio is not set up)
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

- `wifi: could not connect to "X" (reason N, wrong password?)`: that saved
  network refused the password (or timed out); the next one is tried. The
  setup page shows the same under that network and in **Recent connection
  attempts**, no serial needed.
- `wifi: scan: ... 0 of N saved networks in range` then `no saved network
  could be joined, scanning again in 10 s`: none of the saved networks is
  here; it rescans after 10, 20, 40, then every 60 s, forever.
- `sip: ... WiFi lost during the call` / `network changed during the call`:
  the call was ended locally because its network went away.
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
(A grandparent who declines the call no longer reads the same: the Function then says "They didn't answer the phone." and keeps the call past 5 s.)
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
src/main.c            app entry, button task (4 contact buttons: call / hang up; BOOT: test modes / factory reset), factory reset, effective audio mode
src/contact_leds.c/.h one light per contact (available / off / blinking while called)
src/schedule.c/.h     SNTP, per-contact availability evaluation
src/tz_posix.c/.h     POSIX TZ rule math (no global TZ, host-testable)
src/config_store.c/.h runtime config in NVS (WiFi networks and migration, SIP account, contacts and schedules), secrets.h seeding
src/improv.c/.h       Improv Serial on UART0 (WiFi from the browser flasher)
src/setup_server.c/.h setup web page + JSON API, mDNS kidphone.local
src/setup_page.html   the setup page (embedded in flash)
src/twilio_provision.c/.h Twilio self-provisioning task
src/status_led.c/.h   onboard WS2812 status light
src/wifi.c/.h         WiFi manager: up to 5 saved networks, strongest visible first, backoff, scan, Improv try
src/sip.c/.h          minimal SIP UA over UDP: INVITE + digest auth, CANCEL, BYE, NAT handling, call state
src/rtp.c/.h          PCMU RTP sender/receiver tasks, jitter buffer, stream buffer to the audio task
src/usb_audio.c/.h    USB host + UAC driver: hot-plug, descriptor log, format choice, volume/mute
src/audio_engine.c/.h audio task (core 1): mic jitter ring, modes A-D and CALL, prompt playback, stats
src/prompts.c/.h      spoken prompt table and play requests (clips embedded from src/prompts/)
src/prompts/*.pcm     the spoken prompts, 16 kHz mono s16le, made by scripts/gen_prompts.py
src/app_config.h      all tunables (button and LED GPIOs, block size, volumes, buffers, SIP/RTP ports and timers)
src/secrets.h         optional developer override (gitignored; template secrets.h.example)
twilio/voice.js       Twilio Function behind the SIP Domain voice URL (allowlist, echo test, no-answer sentence); embedded and uploaded by provisioning
twilio/no_answer.wav  "They didn't answer the phone." for Twilio's <Play> (8 kHz mu-law, 17 KB, made by scripts/gen_prompts.py); embedded and uploaded as a public asset
scripts/gen_prompts.py regenerates src/prompts/*.pcm (and twilio/no_answer.wav) with Piper TTS (offline, developer only); pass clip names to regenerate only those
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
scripts/build_web_bin.sh --version 0.4.0    # set the manifest version
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
