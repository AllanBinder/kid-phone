#!/usr/bin/env python3
"""
Generate the phone's spoken prompts (src/prompts/*.pcm) offline.

The clips are embedded in the firmware (src/CMakeLists.txt EMBED_FILES and
platformio.ini board_build.embed_files); the phone never runs TTS and never
fetches audio over the network. Rerun this script only when a phrase changes,
then commit the .pcm files.

Usage: gen_prompts.py [stem ...]
  With no arguments every clip is regenerated. Piper output varies slightly
  between runs, so pass only the stems whose phrase changed (for example
  "gen_prompts.py no_answer") to keep the other clips byte-identical.

Voice: Piper TTS, en_US-ljspeech-high
  https://huggingface.co/rhasspy/piper-voices/tree/main/en/en_US/ljspeech/high
  MODEL_CARD: dataset LJ Speech (https://keithito.com/LJ-Speech-Dataset/),
  license: public domain. Trained from scratch on that dataset (not
  fine-tuned from another voice), so the generated clips can be redistributed
  in this open source repo. Pinned to piper-voices revision REVISION below and
  checked by sha256.

Output format: raw little-endian signed 16-bit PCM, mono, 16 kHz, no header.
The audio engine upsamples to the earpiece rate (48 kHz) and copies the
sample to every channel. Levels: active speech RMS TARGET_RMS_DBFS with the
peak held at or below PEAK_CEIL_DBFS, so a prompt sounds about as loud as the
status tones and no louder than the dial tone (350 + 440 Hz at 0.1 each:
-20 dBFS RMS, -14 dBFS peak). The earpiece runs at 100 % volume against a
child's ear: do not raise these.

Twilio copies (TWILIO_ASSETS): some phrases are also spoken by Twilio itself
on the call, in this same voice. Those are written from the same mastered
clip as 8 kHz mono G.711 mu-law WAV (the call's own codec, so Twilio <Play>
does not transcode) into twilio/, embedded in the firmware and uploaded by
provisioning as a public Serverless Asset. Call audio reaches the earpiece
with no extra gain, so the levels match the on-device clip.

Needs: Python 3.10 to 3.13 with piper-tts, and ffmpeg on PATH.
  python3.12 -m venv ~/.venv-piper
  ~/.venv-piper/bin/pip install piper-tts
  ~/.venv-piper/bin/python scripts/gen_prompts.py
The voice (about 110 MB) is downloaded once into ~/.cache/kid_phone_piper.
"""
import array
import hashlib
import math
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import wave

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "src" / "prompts"
CACHE = pathlib.Path(os.environ.get("KIDPHONE_PIPER_CACHE", pathlib.Path.home() / ".cache" / "kid_phone_piper"))

VOICE = "en_US-ljspeech-high"
REVISION = "c10ece1aade47bb51c153c893d14e5bf8e5b7117"
BASE = f"https://huggingface.co/rhasspy/piper-voices/resolve/{REVISION}/en/en_US/ljspeech/high/"
FILES = {
    VOICE + ".onnx": "5d4f08ba6a2a48c44592eed3ce56bf85e9de3dd4e20df90541ae68a8310c029a",
    VOICE + ".onnx.json": "7e1f4634af596d83cca997fb7a931ba80b70f8a316a2655ee69c55365e0ace14",
}

RATE = 16000
LENGTH_SCALE = 1.2      # > 1 = slower; a little slow for a small child
SENTENCE_SILENCE = 0.35  # seconds between the two sentences
LEAD_MS = 120           # silence before the voice starts
TAIL_MS = 250           # silence after it ends
FADE_MS = 5
TARGET_RMS_DBFS = -23.0
PEAK_CEIL_DBFS = -7.0

# File stem -> phrase. The stems are the firmware's clip names (src/prompts.c).
PROMPTS = {
    "time_up": "Time's up! Press the button again to call again.",
    "unavailable": "They can't talk right now. Try again later.",
    "no_answer": "They didn't answer the phone.",
    "no_wifi": "The phone isn't connected. Ask a grown-up for help.",
    "not_ready": "This phone isn't ready yet. Ask a grown-up for help.",
    "cant_call": "Oops, that call didn't work. Let's get a grown-up to help.",
    "sleep_time": "It's sleep time. You can call in the morning.",
}


# Stem -> Twilio Asset copy (the firmware embeds it, see twilio_provision.c).
TWILIO_ASSETS = {
    "no_answer": ROOT / "twilio" / "no_answer.wav",
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch_voice():
    CACHE.mkdir(parents=True, exist_ok=True)
    for name, want in FILES.items():
        dst = CACHE / name
        if not dst.exists() or sha256(dst) != want:
            print(f"downloading {name} ...")
            tmp = dst.with_suffix(dst.suffix + ".part")
            urllib.request.urlretrieve(BASE + name, tmp)
            if sha256(tmp) != want:
                tmp.unlink()
                sys.exit(f"{name}: sha256 mismatch, refusing to use it")
            tmp.replace(dst)
    return CACHE / (VOICE + ".onnx")


def load_voice(model):
    try:
        from piper import PiperVoice
        from piper.phonemize_espeak import ESPEAK_DATA_DIR
    except ImportError:
        sys.exit("piper-tts not installed: run this script with the Python that has piper-tts")
    # espeak-ng silently falls back to its build-time path when the data
    # path is long (deep venvs), so use a copy under the short cache path.
    data = CACHE / "espeak-ng-data"
    if not (data / "phontab").exists():
        shutil.copytree(ESPEAK_DATA_DIR, data, dirs_exist_ok=True)
    return PiperVoice.load(str(model), espeak_data_dir=str(data))


def synth(voice, text, wav):
    from piper import SynthesisConfig
    cfg = SynthesisConfig(length_scale=LENGTH_SCALE)
    rate, pcm = None, b""
    for chunk in voice.synthesize(text, cfg):  # one chunk per sentence
        if rate is not None:
            pcm += bytes(int(rate * SENTENCE_SILENCE) * 2)
        rate = chunk.sample_rate
        pcm += chunk.audio_int16_bytes
    with wave.open(str(wav), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


def to_float16k(wav):
    """ffmpeg: mono, high-pass (the earpiece has no bass), 16 kHz, float32."""
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(wav), "-ac", "1", "-af", "highpass=f=120",
                          "-ar", str(RATE), "-f", "f32le", "-"], check=True, capture_output=True).stdout
    a = array.array("f")
    a.frombytes(raw)
    if sys.byteorder != "little":
        a.byteswap()
    return list(a)


def trim(x, thresh=1e-3):
    start = next((i for i, v in enumerate(x) if abs(v) > thresh), 0)
    end = len(x) - next((i for i, v in enumerate(reversed(x)) if abs(v) > thresh), 0)
    return x[start:end]


def active_rms(x):
    """RMS over 20 ms frames within 30 dB of the loudest frame (speech, not pauses)."""
    n = RATE // 50
    frames = [x[i:i + n] for i in range(0, len(x) - n + 1, n)]
    ms = [sum(v * v for v in f) / n for f in frames]
    top = max(ms)
    act = [m for m in ms if m > top / 1000.0]
    return math.sqrt(sum(act) / len(act))


def master(x):
    x = trim(x)
    gain = 10 ** (TARGET_RMS_DBFS / 20) / active_rms(x)
    peak = max(abs(v) for v in x)
    ceil = 10 ** (PEAK_CEIL_DBFS / 20)
    if peak * gain > ceil:
        gain = ceil / peak
    x = [v * gain for v in x]
    nf = RATE * FADE_MS // 1000
    for i in range(nf):
        x[i] *= i / nf
        x[-1 - i] *= i / nf
    x = [0.0] * (RATE * LEAD_MS // 1000) + x + [0.0] * (RATE * TAIL_MS // 1000)
    pcm = array.array("h", (max(-32767, min(32767, round(v * 32767))) for v in x))
    if sys.byteorder != "little":
        pcm.byteswap()
    return pcm, gain


def write_twilio_asset(pcm, out):
    """16 kHz s16le clip -> 8 kHz mono mu-law WAV, no metadata (reproducible)."""
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ar", str(RATE), "-ac", "1", "-i", "-",
                    "-ar", "8000", "-ac", "1", "-c:a", "pcm_mulaw", "-map_metadata", "-1",
                    "-fflags", "+bitexact", "-flags:a", "+bitexact", str(out)],
                   input=pcm.tobytes(), check=True)


def main():
    stems = sys.argv[1:] or list(PROMPTS)
    unknown = [s for s in stems if s not in PROMPTS]
    if unknown:
        sys.exit(f"unknown clip {', '.join(unknown)}; known: {', '.join(PROMPTS)}")
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg not found on PATH")
    voice = load_voice(fetch_voice())
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    total = 0
    with tempfile.TemporaryDirectory() as tmp:
        for stem in stems:
            text = PROMPTS[stem]
            wav = pathlib.Path(tmp) / (stem + ".wav")
            synth(voice, text, wav)
            pcm, gain = master(to_float16k(wav))
            out = OUT_DIR / (stem + ".pcm")
            out.write_bytes(pcm.tobytes())
            peak = max(abs(v) for v in pcm) / 32768.0
            total += len(pcm) * 2
            print(f"{out.relative_to(ROOT)}: {len(pcm) / RATE:.2f} s, {len(pcm) * 2} bytes, "
                  f"peak {20 * math.log10(peak):.1f} dBFS, gain {20 * math.log10(gain):+.1f} dB  \"{text}\"")
            if stem in TWILIO_ASSETS:
                asset = TWILIO_ASSETS[stem]
                write_twilio_asset(pcm, asset)
                print(f"{asset.relative_to(ROOT)}: {asset.stat().st_size} bytes (8 kHz mu-law WAV for Twilio)")
    print(f"total {total} bytes ({total / 1024:.0f} KiB)")


if __name__ == "__main__":
    main()
