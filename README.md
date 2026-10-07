# kid_phone, stage 1: USB handset audio test

Proof of concept for a toddler phone. Stage 1 proves the audio path only:
an ESP32-S3 acts as USB host to a LIXADA retro telephone handset (a USB Audio
Class device with a USB-C plug), opens its mic and earpiece, and runs four
test modes selected by one button. No WiFi, no SIP yet.

Later stages: SIP client registered to a Twilio SIP Domain, G.711 mu-law
8 kHz RTP, button press calls a grandparent. The resampler and mu-law codec
used by mode B here are the same modules the SIP stage will use.

## Hardware

- Board: ESP32-S3-DevKitC-1 clone, module N16R8 (16 MB flash, 8 MB octal
  PSRAM; GPIO 33-37 belong to the PSRAM and are not used).
- **COM port** (USB-UART bridge, UART0 on GPIO 43/44): flashing, console,
  logs. Connect this one to the Mac.
- **Native USB port** (GPIO 19/20, labelled USB or OTG): USB host. Plug the
  handset in here.
- **IN-OUT solder pad bridged**: required, so the board's 5 V comes out of the
  native port and powers the handset. With the pad bridged, do not plug the
  native port into a computer (it would back-feed 5 V).
- **Mode button**: one lead to **GPIO 4**, the other lead to **GND**. No
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

## Modes

The button cycles A -> B -> C -> D -> A. Boot starts in **C** so plugging the
handset in gives an immediate sound.

| Mode | What it does | What you should hear in the earpiece |
|---|---|---|
| A | Wideband loopback: mic straight to earpiece | Your own voice, full bandwidth, with a small delay (roughly 50 to 80 ms, mostly output buffering). |
| B | Telephone loopback: mic, low-pass, decimate to 8 kHz, G.711 mu-law encode and decode, interpolate back up | Your voice, but narrow and "phone-like": no deep bass, no sibilance sparkle. A faint grain on quiet sounds is the mu-law quantization and is expected. |
| C | US dial tone, 350 Hz + 440 Hz | A steady, slightly beating hum, like picking up a US landline. |
| D | Silence | Nothing (a good check for hiss or USB noise floor). |

Mic data keeps flowing in every mode, so the mic peak in the stats line is
live even during C and D.

## Reading the log

Good boot:

```
I (...) main: Mode button on GPIO 4 (to GND). Starting in mode C, US dial tone (350 + 440 Hz)
I (...) usb_audio: USB host installed (native port, GPIO 19/20)
I (...) usb_audio: UAC class driver installed, waiting for handset ...
```

Handset plugged in (one block per direction):

```
I usb_audio: ==== UAC interface found: EARPIECE (TX, host->device), usb addr 1, interface 2 ====
I usb_audio:   device: VID:PID xxxx:xxxx, manufacturer "...", product "...", serial "..."
I usb_audio:   1 alternate setting(s) with audio:
I usb_audio:     alt 1: format=PCM, 2 ch, 16-bit in 2-byte subframe, rates Hz: 48000
I usb_audio:   chosen: 48000 Hz, 2 ch, 16-bit (2-byte subframe)
I usb_audio:   set volume 60%: ESP_OK, unmute: ESP_OK
I usb_audio:     earpiece volume control: YES, now -12.00 dB ...
I usb_audio: ==== EARPIECE READY ====
I audio: engine: mic yes, earpiece yes, telephone path ready, wideband direct copy
```

Before those, the driver also dumps the raw configuration descriptor
(`CONFIG_UAC_PRINTF_CONFIGURATION_DESCRIPTOR`). Copy the whole block when
reporting results; it answers any later format question.

Every 5 s:

```
I audio: stats 5s: mode C | mic 48000Hz/1ch/16b earpiece 48000Hz/2ch/16b | in 240000 out 240000 frames | underruns 0 overruns 0 txfail 0 | mic peak 812 (-32.1 dBFS) | jitter buf 960
```

- `in` and `out` should both be about rate x 5 (240000 at 48 kHz).
- `underruns` / `overruns` near 0 in steady state; a few right after a mode
  change or plug-in are normal.
- `txfail` should stay 0. Steady nonzero means the earpiece stream is not
  draining.
- `mic peak` should jump when you talk or tap the handset (about -30 dBFS
  quiet room, -10 dBFS or higher when speaking into it).

Failure signs:

- `stats 5s: mode C | no handset connected`: the handset never enumerated.
  Check the IN-OUT pad (no 5 V on the native port), the port (must be the
  native one), and the cable/adapter.
- `no usable format`: the handset only offers something this firmware does
  not handle (8-bit, more than 2 channels). The alt list above it says what.
- `stream start failed`: the driver rejected the chosen alt; send the log.
- `telephone path unavailable`: a rate that is not a multiple of 8 kHz was
  chosen (e.g. 44.1 kHz only); mode B outputs silence in that case.
- On unplug: `EARPIECE disconnected, closing` and `MIC disconnected, closing`,
  then `waiting for handset`. Replugging should print the full open sequence
  again with no reboot.

## Layout

```
src/main.c            app entry, mode state, button task (GPIO 4, polled + debounced)
src/usb_audio.c/.h    USB host + UAC driver: hot-plug, descriptor log, format choice, volume/mute
src/audio_engine.c/.h audio task (core 1): mic jitter ring, modes A-D, stats
src/app_config.h      all tunables (GPIO, block size, volumes, buffer depths)
components/g711/      G.711 mu-law encode/decode (reusable, pure functions)
components/resampler/ integer-ratio FIR decimator/interpolator (reusable)
```
