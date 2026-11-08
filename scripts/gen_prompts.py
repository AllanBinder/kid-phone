#!/usr/bin/env python3
"""
Generate the phone's spoken prompts (src/prompts/*.pcm) offline.

The clips are embedded in the firmware (src/CMakeLists.txt EMBED_FILES and
platformio.ini board_build.embed_files); the phone never runs TTS and never
fetches audio over the network. Rerun this script only when a phrase changes,
then commit the .pcm files.

Usage: gen_prompts.py [--lang en|ca|es|fr|it|nl|de|vi|pt-br] [stem ...]
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
  fr  Kokoro-82M (Apache-2.0), voice ff_siwis (SIWIS database, CC BY 4.0).
  it  Piper it_IT-serena-medium (synthetic data, CC BY 4.0).
  nl  Piper nl_BE-rdh-medium (r-dh/dutch-vl-tts, CC0), trained from scratch.
  de  Piper de_DE-mls-medium speaker 130 (Multilingual LibriSpeech, CC BY 4.0);
      short prompts go through a carrier phrase, see synth_piper_carrier.
  vi  VieNeu-TTS v3 Turbo + MOSS audio tokenizer (Apache-2.0), voice Mỹ Duyên.
  pt-br  Kokoro-82M, voice pf_dora.
  Run fr/pt-br with the Python that has kokoro (~/.venv-kokoro) and vi with
  the one that has vieneu (~/.venv-vieneu). The README table has the
  licences and credits.

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
KOKORO_REPO = "hexgrad/Kokoro-82M"
KOKORO = "https://huggingface.co/" + KOKORO_REPO + "/resolve/f3ff3571791e39611d31c381e3a41a3af07b4987/"
KOKORO_FILES = {
    "config.json": "5abb01e2403b072bf03d04fde160443e209d7a0dad49a423be15196b9b43c17f",
    "kokoro-v1_0.pth": "496dba118d1a58f5f3db2efc88dbdc216e0483fc89fe6e47ee1f2c53f18ad1e4",
}
KOKORO_RATE = 24000
VIENEU = "https://huggingface.co/pnnbao-ump/VieNeu-TTS-v3-Turbo/resolve/61b85e3d937fbbacb387714180e8182823512523/"
MOSS = "https://huggingface.co/OpenMOSS-Team/MOSS-Audio-Tokenizer-Nano-ONNX/resolve/ceff0d0749bfb3fa2d61149794ec6feef0d1e1ae/"
VIENEU_RATE = 48000

RATE = 16000
CARRIER_MAX_WORDS = 7  # "carrier" voices: prompts under this many words
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
    "fr": {
        "engine": "kokoro",
        "dir": "kokoro-82m",
        "base": KOKORO,
        "files": dict(KOKORO_FILES, **{
            "voices/ff_siwis.pt": "8073bf2d2c4b9543a90f2f0fd2144de4ed157e2d4b79ddeb0d5123066171fbc9",
        }),
        "kokoro_lang": "f",  # French (espeak-ng fr-fr)
        "speaker": "ff_siwis",
        "length_scale": 1.1,
        "suffix": "_fr",
        "prompts": {
            "time_up": "C'est fini ! Appuie encore sur le bouton pour rappeler.",
            "unavailable": "Ils ne peuvent pas parler maintenant. Réessaie plus tard.",
            "no_answer": "Personne n'a répondu au téléphone.",
            "no_wifi": "Le téléphone n'est pas connecté. Demande de l'aide à un adulte.",
            "not_ready": "Ce téléphone n'est pas encore prêt. Demande de l'aide à un adulte.",
            "cant_call": "Oups, l'appel n'a pas marché. Allons chercher un adulte pour nous aider.",
            "sleep_time": "C'est l'heure de dormir. Tu pourras appeler demain matin.",
            "secret_start": "Dis ton secret après le bip !",
            "secret_sent": "Chut ! Ton secret est en route.",
        },
    },
    "it": {
        "engine": "piper",
        "voice": "it_IT-serena-medium",
        "base": PIPER + "e7229f7d33a10e145e109b886d661984201ac3c4/it/it_IT/serena/medium/",
        "files": {
            "it_IT-serena-medium.onnx": "fe4e26b2c1236e2a44d2e295cad564d94a0d8b0a9fca1ccdfd70dd4af3116eb5",
            "it_IT-serena-medium.onnx.json": "14cb4995874138b2792ae07753b5540942124761af9ae6609f9b520f28837881",
        },
        "length_scale": 1.1,
        "suffix": "_it",
        "prompts": {
            "time_up": "Il tempo è finito! Premi di nuovo il pulsante per richiamare.",
            "unavailable": "Adesso non possono parlare. Riprova più tardi.",
            "no_answer": "Non hanno risposto al telefono.",
            "no_wifi": "Il telefono non è collegato. Chiedi aiuto a un adulto.",
            "not_ready": "Questo telefono non è ancora pronto. Chiedi aiuto a un adulto.",
            "cant_call": "Ops, la chiamata non è riuscita. Andiamo a chiamare un adulto che ci aiuti.",
            "sleep_time": "È ora di fare la nanna. Puoi chiamare domani mattina.",
            "secret_start": "Racconta il tuo segreto dopo il bip!",
            "secret_sent": "Shh! Il tuo segreto è in viaggio.",
        },
    },
    "nl": {
        "engine": "piper",
        "voice": "nl_BE-rdh-medium",
        "base": PIPER + "e7229f7d33a10e145e109b886d661984201ac3c4/nl/nl_BE/rdh/medium/",
        "files": {
            "nl_BE-rdh-medium.onnx": "59fcf2d6037e1c12386e3d2cf6721c727f286cfeec9db5617407d3bfb4c32559",
            "nl_BE-rdh-medium.onnx.json": "83a5719c3cf138cc5fd4a9ad7d6752982a7fb5ccf94b6b6d4a9637bcee1b5d46",
        },
        "length_scale": 1.1,
        "suffix": "_nl",
        "prompts": {
            "time_up": "De tijd is om! Druk nog een keer op de knop om opnieuw te bellen.",
            "unavailable": "Ze kunnen nu niet praten. Probeer het later nog eens.",
            "no_answer": "Ze hebben de telefoon niet opgenomen.",
            "no_wifi": "De telefoon is niet verbonden. Vraag een volwassene om hulp.",
            "not_ready": "Deze telefoon is nog niet klaar. Vraag een volwassene om hulp.",
            "cant_call": "Oei, het bellen is niet gelukt. Laten we een volwassene halen om te helpen.",
            "sleep_time": "Het is slaaptijd. Je kunt morgenochtend bellen.",
            "secret_start": "Vertel je geheimpje na de piep!",
            "secret_sent": "Sst! Je geheimpje is onderweg.",
        },
    },
    "de": {
        "engine": "piper",
        "voice": "de_DE-mls-medium",
        "base": PIPER + "e7229f7d33a10e145e109b886d661984201ac3c4/de/de_DE/mls/medium/",
        "files": {
            "de_DE-mls-medium.onnx": "69cd1d2aa5a35839a518966fcc4924b5f93e5f8c948ed0752b1a616ad53f65bf",
            "de_DE-mls-medium.onnx.json": "b0af1c89ddfdc72d32e015729b0e89b99eec13c2c8caa1db7488d98e9e570b40",
        },
        "speaker": 130,  # index in speaker_id_map; female, picked from all 236 by F0 + whisper
        "join_sentences": True,  # MLS voices babble on short utterances, see synth_piper
        "carrier": "Das Telefon ist heute leider nicht verbunden",  # see synth_piper
        "noise_scale": 0.3,
        "noise_w": 0.3,
        "length_scale": 1.1,
        "suffix": "_de",
        "prompts": {
            "time_up": "Die Zeit ist um! Drück noch mal auf den Knopf, um wieder anzurufen.",
            "unavailable": "Sie können gerade nicht sprechen. Versuch es später noch mal.",
            "no_answer": "Sie sind nicht ans Telefon gegangen.",
            "no_wifi": "Das Telefon ist nicht verbunden. Frag einen Erwachsenen um Hilfe.",
            "not_ready": "Dieses Telefon ist noch nicht fertig. Frag einen Erwachsenen um Hilfe.",
            "cant_call": "Hoppla, der Anruf hat nicht geklappt. Lass uns einen Erwachsenen holen, der uns hilft.",
            "sleep_time": "Es ist Schlafenszeit. Du kannst morgen früh anrufen.",
            "secret_start": "Erzähl dein Geheimnis nach dem Piep!",
            "secret_sent": "Psst! Dein Geheimnis ist unterwegs.",
        },
    },
    "vi": {
        "engine": "vieneu",
        "dir": "vieneu-v3-turbo",
        "base": VIENEU,
        "files": {
            "onnx_update/config.json": "17d89d414ee302a82db7b330bf57b4cdf8541569392119c81f552178cafcb79b",
            "onnx_update/tokenizer.json": "6cc6bcbe380b8c37bd9f2514e37c5dfa3e00e122c6e3125dae5c4afe48e39158",
            "onnx_update/vieneu_prefill.onnx": "27f8b064f6b57b5448e95d095f1959588c005d614678045c2b97ecccf3b7a0f7",
            "onnx_update/vieneu_decode_step.onnx": "bedc379cea61ea5d616312750d95ad3924e055856662d19187a889a5edc24ceb",
            "onnx_update/vieneu_acoustic_cached.onnx": "f631e3387c788c3d8b9a5ac5df94952af5bc4c4d1049ff8a751e76a246fff2d4",
            "onnx_update/vieneu_backbone_shared.data": "c7c072193db33d0542457e2612c7272c44c4279d1cafaf0aa4c379964911db2f",
            "onnx_update/vieneu_v3_heads.npz": "fb22484baa424bbb775133a6e5f0d00d6299b2b256fbe3312a864b85b9aed01e",
        },
        "sources": [(MOSS, {
            "codec_browser_onnx_meta.json": "3e291c883bb7d11ff2fe8e964e3e495519760358859f35c951254c7741592731",
            "moss_audio_tokenizer_decode_full.onnx": "0fbbafe3fd4afa2a019af5c5ced204af6e2d1db044fa40f021525d2aee95b4ac",
            "moss_audio_tokenizer_decode_step.onnx": "9527c86a29e1837edec1f74db57d5eeaadb3a715af3382703566460afed25855",
            "moss_audio_tokenizer_decode_shared.data": "e69d52e0f4e84ca27850557ee54face46632d3a5a16c89bd246c7c408466dcad",
        }, "moss")],
        "speaker": "Mỹ Duyên",  # preset voice of the vieneu package, picked from 11 by F0 + whisper
        "seed": 20261107,   # np.random seed per sentence: reruns give the same clip
        "suffix": "_vi",
        "length_scale": 1.0,  # not used: the model has no rate control
        "prompts": {
            "time_up": "Hết giờ rồi! Con bấm nút lần nữa để gọi lại nhé.",
            "unavailable": "Bây giờ mọi người chưa nói chuyện được. Con thử lại sau nhé.",
            "no_answer": "Không ai nghe máy cả.",
            "no_wifi": "Điện thoại chưa kết nối mạng. Con nhờ người lớn giúp nhé.",
            "not_ready": "Điện thoại này chưa sẵn sàng. Con nhờ người lớn giúp nhé.",
            "cant_call": "Ôi, cuộc gọi không được rồi. Mình đi tìm người lớn giúp nhé.",
            "sleep_time": "Đến giờ đi ngủ rồi. Sáng mai con gọi nhé.",
            "secret_start": "Con nói bí mật sau tiếng bíp nhé!",
            "secret_sent": "Suỵt! Bí mật của con đang được gửi đi rồi.",
        },
    },
    "pt-br": {
        "engine": "kokoro",
        "dir": "kokoro-82m",
        "base": KOKORO,
        "files": dict(KOKORO_FILES, **{
            "voices/pf_dora.pt": "07e4ff987c5d5a8c3995efd15cc4f0db7c4c15e881b198d8ab7f67ecf51f5eb7",
        }),
        "kokoro_lang": "p",  # Brazilian Portuguese (misaki + espeak-ng pt-br)
        "speaker": "pf_dora",
        "suffix": "_pt_br",
        "length_scale": 1.1,
        "prompts": {
            "time_up": "Acabou o tempo! Aperte o botão de novo para ligar outra vez.",
            "unavailable": "Agora não dá para conversar. Tente de novo mais tarde.",
            "no_answer": "Ninguém atendeu o telefone.",
            "no_wifi": "O telefone não está conectado. Peça ajuda a um adulto.",
            "not_ready": "Este telefone ainda não está pronto. Peça ajuda a um adulto.",
            "cant_call": "Ops, a ligação não funcionou. Vamos chamar um adulto para ajudar.",
            "sleep_time": "É hora de dormir. Você pode ligar amanhã de manhã.",
            "secret_start": "Conte o seu segredo depois do bipe!",
            "secret_sent": "Psiu! O seu segredo já está a caminho.",
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
    """Download the pinned files once (sha256 checked) and return the model
    path (piper, matxa) or the voice's cache folder (kokoro, vieneu). A
    language with "dir" keeps its files in CACHE/<dir>/ at their repo paths;
    "sources" lists more (base, files, subfolder) for a model spread over
    several repos."""
    root = CACHE / lang.get("dir", "")
    for base, files, sub in [(lang["base"], lang["files"], "")] + lang.get("sources", []):
        for name, want in files.items():
            dst = root / sub / name
            dst.parent.mkdir(parents=True, exist_ok=True)
            if not dst.exists() or sha256(dst) != want:
                print(f"downloading {name} ...")
                tmp = dst.with_suffix(dst.suffix + ".part")
                urllib.request.urlretrieve(base + name, tmp)
                if sha256(tmp) != want:
                    tmp.unlink()
                    sys.exit(f"{name}: sha256 mismatch, refusing to use it")
                tmp.replace(dst)
    if lang["engine"] in ("kokoro", "vieneu"):
        return root
    return root / (lang["voice"] + ".onnx")


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
    cfg = SynthesisConfig(speaker_id=lang.get("speaker"), length_scale=lang["length_scale"],
                          noise_scale=lang.get("noise_scale"), noise_w_scale=lang.get("noise_w"))
    if lang.get("join_sentences"):
        # The MLS voices (trained on long audiobook segments) babble when a
        # sentence is short on its own ("Frag einen Erwachsenen um Hilfe."),
        # so speak the whole prompt as one sentence, with a comma pause.
        parts = sentences(text)
        text = " ".join([re.sub(r"[.!?]+$", ",", t) for t in parts[:-1]] + parts[-1:])
    if lang.get("carrier") and len(text.split()) < CARRIER_MAX_WORDS:
        return synth_piper_carrier(voice, lang, cfg, text, wav)
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


def synth_piper_carrier(voice, lang, cfg, text, wav):
    """A whole prompt this short still babbles in the MLS voices ("Sie sind
    nicht ans Telefon gegangen." comes out as noise words), however many
    takes. Speak it after a carrier phrase in one breath ("<carrier>,
    <prompt>"), then cut at the start of the comma pause: the quiet run (2 % of
    peak, 10 ms frames, at least 50 ms) that starts nearest to where the
    carrier said on its own stops. Takes vary: check the clip with Whisper or
    by ear and rerun that stem if a carrier word is left or the first word
    is clipped."""
    import numpy as np

    def say(t):
        chunks = list(voice.synthesize(t, cfg))
        return np.concatenate([c.audio_float_array for c in chunks]), chunks[0].sample_rate

    alone, rate = say(lang["carrier"] + ".")
    y, rate = say(lang["carrier"] + ", " + text)
    hop = rate // 100
    env = np.array([np.sqrt(np.mean(y[i:i + hop] ** 2)) for i in range(0, len(y) - hop, hop)])
    quiet = env < 0.02 * env.max()
    # where the carrier's speech ends when it is said on its own
    loud = np.nonzero(np.abs(alone) > 0.02 * np.abs(alone).max())[0]
    end = loud[-1] // hop if len(loud) else len(alone) // hop
    runs, i = [], 0
    while i < len(quiet):
        if quiet[i]:
            j = i
            while j < len(quiet) and quiet[j]:
                j += 1
            if j - i >= 5 and j < len(quiet):  # 50 ms or more, not the tail
                runs.append(i)
            i = j
        else:
            i += 1
    if not runs:
        sys.exit(f"carrier: no pause found after \"{lang['carrier']}\" for \"{text}\"; rerun")
    start = min(runs, key=lambda r: abs(r - end))
    cut = start * hop
    with wave.open(str(wav), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes((np.clip(y[cut:], -1, 1) * 32767).astype("<i2").tobytes())


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


def sentences(text):
    return [t.strip() for t in re.findall(r"[^.!?]+[.!?]*", text) if t.strip()]


def write_float_wav(wav, rate, parts):
    """Sentences joined by SENTENCE_SILENCE, as 16-bit WAV."""
    import numpy as np
    gap = np.zeros(int(rate * SENTENCE_SILENCE), dtype=np.float32)
    out = []
    for i, a in enumerate(parts):
        out += ([gap] if i else []) + [np.asarray(a, dtype=np.float32).reshape(-1)]
    audio = np.clip(np.concatenate(out), -1.0, 1.0)
    with wave.open(str(wav), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes((audio * 32767).astype("<i2").tobytes())


def load_kokoro(root, lang):
    os.environ["HF_HUB_OFFLINE"] = "1"  # everything it needs is pinned in root
    try:
        from kokoro import KModel, KPipeline
    except ImportError:
        sys.exit("kokoro not installed: run this script with the Python that has kokoro "
                 "(see the docstring, ~/.venv-kokoro)")
    model = KModel(repo_id=KOKORO_REPO, config=str(root / "config.json"),
                   model=str(root / "kokoro-v1_0.pth")).eval()
    pipe = KPipeline(lang_code=lang["kokoro_lang"], repo_id=KOKORO_REPO, model=model)
    return pipe, pipe.load_voice(str(root / "voices" / (lang["speaker"] + ".pt")))


def synth_kokoro(voice, lang, text, wav):
    pipe, pack = voice
    parts = []
    for sentence in sentences(text):
        # Kokoro "speed" is a rate (> 1 = faster): the inverse of length_scale.
        audio = [r.audio.numpy() for r in pipe(sentence, voice=pack, speed=1.0 / lang["length_scale"])]
        parts.append(__import__("numpy").concatenate(audio))
    write_float_wav(wav, KOKORO_RATE, parts)


def load_vieneu(root, lang):
    os.environ["HF_HUB_OFFLINE"] = "1"  # everything it needs is pinned in root
    try:
        from vieneu import Vieneu
        from vieneu._v3_turbo_engine import onnx_runtime_lite
    except ImportError:
        sys.exit("vieneu not installed: run this script with the Python that has vieneu "
                 "(see the docstring, ~/.venv-vieneu)")
    # The engine fetches the MOSS codec from the Hub (unpinned) unless it is
    # given codec_dir, which V3TurboVieNeuTTS does not pass on: inject it.
    init = onnx_runtime_lite.OnnxV3LiteEngine.__init__

    def pinned_init(self, *a, **k):
        k["codec_dir"] = str(root / "moss")
        init(self, *a, **k)
    onnx_runtime_lite.OnnxV3LiteEngine.__init__ = pinned_init
    return Vieneu(mode="v3turbo", backend="onnx", backbone_repo=str(root),
                  onnx_dir=str(root / "onnx_update"))


def synth_vieneu(tts, lang, text, wav):
    import numpy as np
    parts = []
    for sentence in sentences(text):
        # The model samples its speech codes (np.random): seed every
        # sentence so a rerun gives the same clip. No audio watermark: the
        # clip is resampled to 16 kHz and mu-law, which would not keep it.
        np.random.seed(lang["seed"])
        parts.append(tts.infer(sentence, voice=lang["speaker"], apply_watermark=False))
    write_float_wav(wav, VIENEU_RATE, parts)


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
    elif lang["engine"] == "kokoro":
        voice, synth = load_kokoro(model, lang), synth_kokoro
    elif lang["engine"] == "vieneu":
        voice, synth = load_vieneu(model, lang), synth_vieneu
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
