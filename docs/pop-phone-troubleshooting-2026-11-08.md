# Native Union POP Phone: silent earpiece (open, 2026-11-08)

A builder (Discord: CodyIsChatting) set up a kid phone with a **Native Union POP
Phone** (USB-C, about $40 on Amazon) instead of the LIXADA handset. The other
side hears him; he hears nothing on the handset: no ringback, no Twilio
prompts, no **Play a sample**. The handset works fine as a speaker on a Mac
and on an iPhone.

## What the logs proved

- **Not the network.** RTP arrives at about 50 pkt/s with 0 loss, and the
  phone latches onto Twilio's real media address. Ignore the Eero port
  forwards (the Twilio 10000-60000 range is Twilio's side; it should be
  removed from his router).
- **Firmware sends audio to the earpiece with no errors.** The prompt runs
  (`mode D -> PROMPT -> D`), 48000 frames/s go out, `txfail 0`, no underruns.
  Sound is lost between the ESP32 USB host and the handset's speaker.
- **Descriptors** (first log, plug-in at 21:03:53):
  - VID:PID `2d1d:000f`, product "Native Union POP Phone", UAC 1.0, 4 interfaces
    (AC, speaker AS, mic AS, HID for the hook button; HID is never opened).
  - Speaker (interface 1, EP 0x03 OUT isoc, MPS 288): one alt only, **48 kHz,
    2 ch, 24-bit in 3-byte subframes**. Feature unit: master mute (ch0),
    volume on ch1/ch2 only. Set volume 100% and unmute both returned OK; reads
    back -0.94 dB, unmuted.
  - Mic (interface 2, EP 0x83 IN, MPS 192): 48 kHz, 2 ch, 16-bit. No volume
    or mute control (the "not supported" errors are harmless).
  - Earpiece opens before the mic.
- **Open oddities:**
  - The second log (21:48, sample + test call) reports `earpiece 48000Hz/2ch/16b`,
    which the POP Phone's descriptor doesn't offer. Unknown whether a different
    handset was plugged in. Asked, no answer yet.
  - Mic peak stayed about -73 dBFS during the test call (maybe nobody spoke).

## Ruled out in code

- 24-bit packing in `src/audio_engine.c` `write_sample` (LE, top 16 bits in
  bytes 1-2) is correct.
- UAC driver (espressif/usb_host_uac 1.5.0) sizes OUT packets right (288 B).
- FIFO: firmware sets a custom split (`fifo_settings_custom`), so
  `CONFIG_USB_HOST_HW_BUFFER_BIAS_*` does nothing. Standard split gives the
  earpiece 384 B, more than the POP's 288 B packet. The LIXADA (MPS 384, 16-bit) works.
- The iPhone "Headphones vs Other Device" thing in Native Union's docs is
  iOS-only. The handset has no mode or reset button.

## Tools shipped for remote troubleshooting (0.8.0, live 2026-11-08)

Setup page, **Advanced > Handset troubleshooting** (`POST /api/handset`,
NVS keys `hs_layout` / `hs_flags`, applied at restart):

- Info line: handset name, USB ID, earpiece and mic formats.
- **Play a test tone**: 3 s, 1 kHz, half scale. Idle only.
- **USB buffer layout**: 0 Standard (RX 88 / PTX 96 / NPTX 16 lines), 1 bigger
  earpiece (RX 56 / PTX 128 / NPTX 16).
- **Speaker only test**: mic never opened.
- **Leave the earpiece volume** as the handset sets it (no SET_CUR volume/mute).
- **Detailed USB log**: uac-host and USBH at debug.

Not hardware-tested at release (no phone on the Mac). Regression-check the
LIXADA with defaults.

## Session plan with Cody (one change at a time, test tone after each)

1. Reinstall 0.8.0 (the browser install erases settings: redo WiFi and Twilio,
   Auth Token included). Screenshot of the troubleshooting section, then test tone.
2. Detailed USB log on, Save and restart, Logs & Console, test tone, send the log.
3. Speaker only test. If the tone plays, the problem is running both
   directions at once.
4. Bigger earpiece buffer (speaker only off).
5. Leave earpiece volume.
6. Also free: a BOOT short press while idle cycles test modes A-D (C = dial tone).

If one combination works, make it the automatic default for `2d1d:000f` so
other POP Phone owners need nothing. If none works, next ideas: send
SET_CUR sample rate to EP 0x03 even though it isn't advertised, open the
HID interface (hook button), try opening the mic before the earpiece, or
patch the UAC driver to log per-packet isoc OUT status.
