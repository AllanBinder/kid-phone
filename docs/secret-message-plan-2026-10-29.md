# Secret message: plan (2026-10-29)

Kid records a voice note on the handset; the phone emails it as a WAV attachment
straight from the device over SMTP. Nothing passes through our servers or Twilio.

Depends on the pin assignment round (0.5.0: assignable GPIOs, up to N contact
slots, optional lamps). Build on top of that, not in parallel: both touch the
button handler, config_store and the setup page.

## Decisions (founder, 2026-10-29)
- Option A: on-device recording + direct SMTP email (parent's own account, app password).
- Secret is a per-button toggle, so a parent can pick either:
  - **dedicated**: a button whose only job is secrets (tap = record), no phone number needed;
  - **long press**: a normal contact button; short press calls, hold records a secret for that person.
- Delivery is email.
- Max length configurable, default 60 s.

## Kid flow
1. Trigger (tap on a dedicated button, or hold ~1.5 s on a long-press button).
2. Prompt: "Tell <name> a secret after the beep!" then beep. (Piper clip; recipient name
   is not spoken, so the clip is generic: "Tell your secret after the beep!")
3. Recording: that button's lamp blinks (if it has one). Stops on a press of the same
   button or at the max length (short "time's up" beep).
4. Prompt: "Shh! Your secret is on its way." Send runs in the background.
5. Under 2 s of audio: discard silently, no email.

## Button behaviour
- secret off: unchanged (act on press, as today).
- secret = long press: the button acts on RELEASE for calls (< hold threshold) and starts a
  secret once held past the threshold. Note this changes the 0.3.0 "act on press" feel for
  those buttons only.
- secret = dedicated: tap records. Slot needs a button pin and a recipient email, no number;
  its lamp (if any) is on whenever secrets are possible.
- Ignored during a call, while ringing, before WiFi/setup, and during cooldown.

## Audio
- Handset mic is already streaming in calls (usb_audio, 48 kHz stereo). Start mic + earpiece
  without a SIP call (check usb_audio supports that standalone).
- Downmix to mono, decimate 48k -> 16k (simple low-pass FIR), 16-bit PCM WAV.
- Buffer in PSRAM (8 MB octal, enabled): 32 KB/s, 60 s ~ 1.9 MB; allocate max_len up front.
- Optional: gentle auto-gain / normalize before send (toddlers whisper "secrets").

## Email
- SMTP over TLS (implicit 465 or STARTTLS 587), AUTH LOGIN/PLAIN, esp-tls + mbedtls.
- MIME multipart: short text body + `secret-YYYYMMDD-HHMM.wav`, base64 streamed in chunks
  from PSRAM (no second full-size buffer).
- Subject: "🤫 A secret from <kid name>, 7:42 pm" (local time in home TZ).
- One queued secret held in PSRAM; retry with backoff while WiFi/SMTP is down. Lost on
  power loss (acceptable, document it).
- Errors surface on the setup page Status ("Last secret: sent 7:42 pm" / "failed: login
  rejected"), and a "Send test email" button in the Email card.

## Limits and settings
Global (setup page "Secret messages" card):
- SMTP host, port, security, username, app password (write-only in the UI), from name.
- Kid's name (used in the subject).
- Max length: 10-120 s, default 60.
- Hourly cap: default 5; over the cap, the button plays the busy tone.
- Allowed at bedtime: on/off, default off.
Per slot: secret mode (off / long press / dedicated) + recipient email.

Guide copy for parents: Gmail needs 2-Step Verification on, then an app password
(myaccount.google.com/apppasswords). Some providers (likely Outlook.com) no longer allow
SMTP app passwords; any provider with SMTP login works.

## NVS
New keys (<= 15 chars): smtp_host, smtp_port, smtp_sec, smtp_user, smtp_pass, smtp_from,
kid_name, sec_max_s, sec_cap_hr, sec_bed, and per slot c<N>_sec (u8 mode), c<N>_mail.
Absent keys = feature off; 0.5.0 phones upgrade unchanged.

## New prompts (Piper en_US-ljspeech-high, ~/.venv-piper)
- secret_start: "Tell your secret after the beep!"
- secret_sent: "Shh! Your secret is on its way."
- secret_off (no email set up / over cap): reuse cant_call or busy tone.

## Open
- Does the AB17X handset mic open cleanly with no earpiece stream? Bench check.
- Hook switch is still not in firmware; without it, recording starts with the handset on
  the wall. Fine for v1.
- Version: 0.6.0.
