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

## 2026-11-09: Cody's Windows data (USB Tree Viewer + USBPcap)

- Windows gets a **276-byte** config: IAD; speaker alt 1 = 24-bit and alt 2 = 16-bit,
  both 44.1/48 kHz, EP 0x03 **synchronous** with SamplingFreq control; no selector unit.
  The ESP32 got **226 bytes** on 2026-11-07 21:03 (one 24-bit 48 kHz alt, adaptive EP,
  no SF control, extra selector unit 7, AC header wTotalLength 72 but 79 bytes of units).
  Device descriptor is the same in both (bcdUSB 1.10, MPS0 64, bcdDevice 0x0029).
- **The ESP32 has probably already seen the Windows profile.** The 21:48 log is the same
  boot (uptime 2647 s vs 1031 s) and reports `earpiece 48000Hz/2ch/16b`. Only the
  276-byte profile has a 16-bit speaker alt, and `pick_format` prefers 16-bit. In that
  profile the UAC driver already sends SET_CUR SAMPLING_FREQ 48000 to EP 0x03
  (`uac_host_interface_resume`, same bytes Windows sends). If the earpiece was still
  silent then, profile choice and the SF request are not the fix. Needs confirming:
  ask Cody for the plug-in part of a log showing the 16b case (wTotalLength, "Set EP 3
  frequency") and whether that session was silent too.
- The pcap is incomplete for playback: no isochronous packets at all and no
  SET_INTERFACE alt 1/2 on interface 1. It ends 1.2 s after the SET_CUR. USBPcap also
  cannot see the hub's pre-address enumeration, so Windows' 64-byte first read is known
  Windows behavior, not something this capture shows. The 9-then-276 config reads in it
  come from the client driver after enumeration.
- HID report descriptor (65 B) is input-only: consumer keys, Hook Switch (0x0B/0x20),
  Phone Mute (0x0B/0x2F). No output reports, so there is no host "off-hook" or ring
  signal that could gate the speaker. Opening HID is cheap but unlikely to matter.
- Windows reads mic volume on FU 5 ch0 fine. The ESP driver's "mic volume not supported"
  is likely because the 226 profile routes FU5 -> SU7 -> OT6 and the driver doesn't
  follow the selector. Harmless for now.
- ENUM_SHORT_DESC_REQ_LEN experiment (IDF 5.5.3 `components/usb/enum.c`): one constant
  drives both short reads and the exact-length checks (`expect_num_bytes`). A 64-byte
  device read gets a short 18-byte reply, so that check must be relaxed too. Medium
  effort; do it only if the 16b session turns out to have been audible.
