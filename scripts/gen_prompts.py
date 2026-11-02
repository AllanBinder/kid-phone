#!/usr/bin/env python3
"""
Generate the phone's spoken prompts (src/prompts/*.pcm) offline.

The clips are embedded in the firmware (src/CMakeLists.txt EMBED_FILES and
platformio.ini board_build.embed_files); the phone never runs TTS and never
fetches audio over the network. Rerun this script only when a phrase changes,
then commit the .pcm files.

Usage: gen_prompts.py [--lang en|ca|es] [stem ...]
  --lang picks the language (default en). English clips are
  src/prompts/<stem>.pcm, the others src/prompts/<stem>_<lang>.pcm.
  With no stems every clip of that language is regenerated. TTS output varies
  slightly between runs, so pass only the stems whose phrase changed (for
  example "gen_prompts.py --lang es no_answer") to keep the other clips
  byte-identical.

Voices. Each one is trained from scratch on openly licensed recordings (not
fine-tuned from another voice), so the generated clips can be redistributed
in this open source repo. Each is pinned to a revision and checked by sha256.

  en  Piper TTS, en_US-ljspeech-high
      https://huggingface.co/rhasspy/piper-voices/tree/main/en/en_US/ljspeech/high
      Dataset LJ Speech (https://keithito.com/LJ-Speech-Dataset/), public domain.
  es  Piper TTS, es_ES-carlfm-x_low
      https://huggingface.co/rhasspy/piper-voices/tree/main/es/es_ES/carlfm/x_low
      Dataset carlfm01 (https://github.com/carlfm01/my-speech-datasets), public
      domain.
  ca  Matxa-TTS v2 central Catalan (graphemes), with the WaveNext vocoder in
      the same ONNX file, by the Barcelona Supercomputing Center (Projecte Aina
      / ILENIA). Speaker "ona" (Festcat).
      https://huggingface.co/BSC-LT/matxa-tts-v2-ca-central-graphemes
      Model GPL-3.0, vocoder Apache-2.0. Pre-trained on Catalan Common Voice
      (CC0), fine-tuned on Festcat (UPC, CC BY-SA 3.0 ES) and OpenSLR 69
      (Google, CC BY-SA 4.0). The voice is Festcat's, so the Catalan clips are
      CC BY-SA 3.0 ES (credit: Festcat, Universitat Politecnica de Catalunya;
      model by BSC). See README "Spoken prompts".

Output format: raw little-endian signed 16-bit PCM, mono, 16 kHz, no header.
The audio engine upsamples to the earpiece rate (48 kHz) and copies the
sample to every channel. Levels: active speech RMS TARGET_RMS_DBFS with the
peak held at or below PEAK_CEIL_DBFS, so a prompt sounds about as loud as the
status tones and no louder than the dial tone (350 + 440 Hz at 0.1 each:
-20 dBFS RMS, -14 dBFS peak). The earpiece runs at 100 % volume against a
child's ear: do not raise these.

Twilio copies (twilio): some phrases are also spoken by Twilio itself on the
call, in this same voice. Those are written from the same mastered clip as
8 kHz mono G.711 mu-law WAV (the call's own codec, so Twilio <Play> does not
transcode) into twilio/, embedded in the firmware and uploaded by
provisioning as public Serverless Assets. Call audio reaches the earpiece
with no extra gain, so the levels match the on-device clip.

Needs: Python 3.10 to 3.13 with piper-tts (it brings onnxruntime and numpy,
which the Catalan model uses too), and ffmpeg on PATH.
  python3.12 -m venv ~/.venv-piper
  ~/.venv-piper/bin/pip install piper-tts
  ~/.venv-piper/bin/python scripts/gen_prompts.py --lang ca
Voices are downloaded once into ~/.cache/kid_phone_piper (en about 110 MB,
es about 20 MB, ca about 270 MB).
"""
import array
import hashlib
import math
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import wave

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "src" / "prompts"
CACHE = pathlib.Path(os.environ.get("KIDPHONE_PIPER_CACHE", pathlib.Path.home() / ".cache" / "kid_phone_piper"))

PIPER = "https://huggingface.co/rhasspy/piper-voices/resolve/"
MATXA = "https://huggingface.co/BSC-LT/matxa-tts-v2-ca-central-graphemes/resolve/"

RATE = 16000
SENTENCE_SILENCE = 0.35  # seconds between the two sentences
LEAD_MS = 120           # silence before the voice starts
TAIL_MS = 250           # silence after it ends
FADE_MS = 5
TARGET_RMS_DBFS = -23.0
PEAK_CEIL_DBFS = -7.0

# Per language: engine, pinned voice files (name -> sha256), the clip file
# suffix, the speaking rate (length_scale > 1 = slower; a little slow for a
# small child) and the phrases. File stem -> phrase; the stems are the
# firmware's clip names (src/prompts.c) and must match across languages.
LANGS = {
    "en": {
        "engine": "piper",
        "voice": "en_US-ljspeech-high",
        "base": PIPER + "c10ece1aade47bb51c153c893d14e5bf8e5b7117/en/en_US/ljspeech/high/",
        "files": {
            "en_US-ljspeech-high.onnx": "5d4f08ba6a2a48c44592eed3ce56bf85e9de3dd4e20df90541ae68a8310c029a",
            "en_US-ljspeech-high.onnx.json": "7e1f4634af596d83cca997fb7a931ba80b70f8a316a2655ee69c55365e0ace14",
        },
        "suffix": "",
        "length_scale": 1.2,
        "prompts": {
            "time_up": "Time's up! Press the button again to call again.",
            "unavailable": "They can't talk right now. Try again later.",
            "no_answer": "They didn't answer the phone.",
            "no_wifi": "The phone isn't connected. Ask a grown-up for help.",
            "not_ready": "This phone isn't ready yet. Ask a grown-up for help.",
            "cant_call": "Oops, that call didn't work. Let's get a grown-up to help.",
            "sleep_time": "It's sleep time. You can call in the morning.",
            "secret_start": "Tell your secret after the beep!",
            "secret_sent": "Shh! Your secret is on its way.",
        },
    },
    "es": {
        "engine": "piper",
        "voice": "es_ES-carlfm-x_low",
        "base": PIPER + "b4e358ffcb232591bf73a540e70f7de528e4174a/es/es_ES/carlfm/x_low/",
        "files": {
            "es_ES-carlfm-x_low.onnx": "d69677323a907cd4963f42b29c20a98b5d6bfa7f3e64df339915e4650c00d125",
            "es_ES-carlfm-x_low.onnx.json": "d9bdfa9ff01eb2bc9e62e7d2593939d1e4c4d8eb7cf75f972731539d12399966",
        },
        "suffix": "_es",
        "length_scale": 1.15,
        "prompts": {
            "time_up": "¡Se acabó el tiempo! Pulsa el botón otra vez para volver a llamar.",
            "unavailable": "Ahora no pueden hablar. Inténtalo más tarde.",
            "no_answer": "No han contestado al teléfono.",
            "no_wifi": "El teléfono no está conectado. Pide ayuda a un adulto.",
            "not_ready": "Este teléfono todavía no está listo. Pide ayuda a un adulto.",
            "cant_call": "Uy, la llamada no ha funcionado. Vamos a buscar a un adulto para que nos ayude.",
            "sleep_time": "Es hora de dormir. Puedes llamar por la mañana.",
            "secret_start": "¡Cuenta tu secreto después del pitido!",
            "secret_sent": "¡Chis! Tu secreto ya va de camino.",
        },
    },
    "ca": {
        "engine": "matxa",
        "voice": "matxa_v2_graphemes_10_steps_wavenext",
        "base": MATXA + "c4d30164239fbf9525b1fdc2adb3af637d2fe275/",
        "files": {
            "matxa_v2_graphemes_10_steps_wavenext.onnx":
                "38164ca12c0d4b1e8efc33494547442f81f2aa4ff437b0073144837e58cde47f",
        },
        "speaker": 5,  # "ona" in the model's spk_to_id.json (BSC-LT/matxa-tts-v2 Space)
        "suffix": "_ca",
        "length_scale": 1.1,
        "prompts": {
            "time_up": "S'ha acabat el temps! Torna a prémer el botó per tornar a trucar.",
            "unavailable": "Ara no poden parlar. Torna-ho a provar més tard.",
            "no_answer": "No han agafat el telèfon.",
            "no_wifi": "El telèfon no està connectat. Demana ajuda a un adult.",
            "not_ready": "Aquest telèfon encara no està a punt. Demana ajuda a un adult.",
            "cant_call": "Ups, la trucada no ha funcionat. Anem a buscar un adult perquè ens ajudi.",
            "sleep_time": "És hora de dormir. Pots trucar demà al matí.",
            "secret_start": "Explica el teu secret després del senyal!",
            "secret_sent": "Xxt! El teu secret ja és de camí.",
        },
    },
}

# Stems with a Twilio Asset copy (the firmware embeds it, see
# twilio_provision.c): twilio/<stem><suffix>.wav.
TWILIO_ASSETS = ("no_answer",)

# Matxa v2 graphemes input alphabet, in model id order (symbols_graphemes in
# the BSC Matcha-TTS fork, matcha/text/symbols.py, branch v2-graphemes).
MATXA_SYMBOLS = (["_"] + list(";:,.!?¡¿\u2014…\"«»“”()- ")
                 + list("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
                 + list("ɑɐɒæɓʙβɔɕçɗɖðʤəɘɚɛɜɝɞɟʄɡɠɢʛɦɧħɥʜɨɪʝɭɬɫɮʟɱɯɰŋɳɲɴøɵɸθœɶʘɹɺɾɻʀʁɽʂʃʈʧʉʊʋⱱʌɣɤʍχʎʏʑʐʒʔʡʕʢǀǁǂǃˈˌːˑʼʴʰʱʲʷˠˤ˞↓↑→↗↘'̩'ᵻ")
                 + list("àáèéìíòóùú·üïöñ’#´"))
MATXA_RATE = 22050
MATXA_TEMPERATURE = 0.667


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch_voice(lang):
    CACHE.mkdir(parents=True, exist_ok=True)
    for name, want in lang["files"].items():
        dst = CACHE / name
        if not dst.exists() or sha256(dst) != want:
            print(f"downloading {name} ...")
            tmp = dst.with_suffix(dst.suffix + ".part")
            urllib.request.urlretrieve(lang["base"] + name, tmp)
            if sha256(tmp) != want:
                tmp.unlink()
                sys.exit(f"{name}: sha256 mismatch, refusing to use it")
            tmp.replace(dst)
    return CACHE / (lang["voice"] + ".onnx")


def load_piper(model):
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


def synth_piper(voice, lang, text, wav):
    from piper import SynthesisConfig
    cfg = SynthesisConfig(length_scale=lang["length_scale"])
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


def matxa_clean(sentence):
    """The model's training text cleaner for these phrases (BSC cleancat with
    punctuation and capitals off): lowercase, drop punctuation, keep word
    apostrophes, hyphens and the l·l dot. It has no number or abbreviation expansion, so
    phrases must spell those out."""
    s = sentence.lower().replace("’", "'")
    s = re.sub(r"[^\w'·-]", " ", s)
    s = re.sub(r"\s+", " ", s).strip()
    bad = sorted({c for c in s if c not in MATXA_SYMBOLS})
    if bad:
        sys.exit(f"Catalan model has no symbol for {bad!r} in {sentence!r}")
    return s


def load_matxa(model):
    try:
        import onnxruntime
    except ImportError:
        sys.exit("onnxruntime not installed: run this script with the Python that has piper-tts")
    return onnxruntime.InferenceSession(str(model), providers=["CPUExecutionProvider"])


def synth_matxa(model, lang, text, wav):
    import numpy as np
    ids = {s: i for i, s in enumerate(MATXA_SYMBOLS)}
    pcm = []
    for sentence in re.findall(r"[^.!?]+[.!?]*", text):
        clean = matxa_clean(sentence)
        if not clean:
            continue
        seq = [0]
        for c in clean:
            seq += [ids[c], 0]  # blank between symbols, as in training
        x = np.array([seq], dtype=np.int64)
        out = model.run(None, {
            "x": x,
            "x_lengths": np.array([x.shape[1]], dtype=np.int64),
            "scales": np.array([MATXA_TEMPERATURE, lang["length_scale"]], dtype=np.float32),
            "spks": np.array([lang["speaker"]], dtype=np.int64),
        })
        if pcm:
            pcm.append(np.zeros(int(MATXA_RATE * SENTENCE_SILENCE), dtype=np.float32))
        pcm.append(out[1].reshape(-1).astype(np.float32))
    audio = np.clip(np.concatenate(pcm), -1.0, 1.0)
    with wave.open(str(wav), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(MATXA_RATE)
        w.writeframes((audio * 32767).astype("<i2").tobytes())


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
    args = sys.argv[1:]
    code = "en"
    if args[:1] == ["--lang"]:
        if len(args) < 2 or args[1] not in LANGS:
            sys.exit(f"--lang needs one of {', '.join(LANGS)}")
        code, args = args[1], args[2:]
    lang = LANGS[code]
    prompts = lang["prompts"]
    stems = args or list(prompts)
    unknown = [s for s in stems if s not in prompts]
    if unknown:
        sys.exit(f"unknown clip {', '.join(unknown)}; known: {', '.join(prompts)}")
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg not found on PATH")
    model = fetch_voice(lang)
    if lang["engine"] == "piper":
        voice, synth = load_piper(model), synth_piper
    else:
        voice, synth = load_matxa(model), synth_matxa
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    total = 0
    with tempfile.TemporaryDirectory() as tmp:
        for stem in stems:
            text = prompts[stem]
            wav = pathlib.Path(tmp) / (stem + ".wav")
            synth(voice, lang, text, wav)
            pcm, gain = master(to_float16k(wav))
            out = OUT_DIR / (stem + lang["suffix"] + ".pcm")
            out.write_bytes(pcm.tobytes())
            peak = max(abs(v) for v in pcm) / 32768.0
            total += len(pcm) * 2
            print(f"{out.relative_to(ROOT)}: {len(pcm) / RATE:.2f} s, {len(pcm) * 2} bytes, "
                  f"peak {20 * math.log10(peak):.1f} dBFS, gain {20 * math.log10(gain):+.1f} dB  \"{text}\"")
            if stem in TWILIO_ASSETS:
                asset = ROOT / "twilio" / (stem + lang["suffix"] + ".wav")
                write_twilio_asset(pcm, asset)
                print(f"{asset.relative_to(ROOT)}: {asset.stat().st_size} bytes (8 kHz mu-law WAV for Twilio)")
    print(f"total {total} bytes ({total / 1024:.0f} KiB)")


if __name__ == "__main__":
    main()
