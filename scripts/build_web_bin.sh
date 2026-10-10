#!/usr/bin/env bash
# Build a single merged image for the browser installer (ESP Web Tools).
#
# Usage:
#   scripts/build_web_bin.sh            # merge the existing .pio build artifacts
#   scripts/build_web_bin.sh --build    # run `pio run` first, then merge
#   scripts/build_web_bin.sh --version 0.3.0   # override the manifest version
#
# Output: web/firmware/kid_phone.bin (flashed at offset 0) and the version
# field of web/manifest.json updated in place.
#
# Offsets are not hardcoded where the build decides them: the bootloader and
# partition table offsets come from the generated sdkconfig, and the app (and
# otadata, if present) offsets come from partitions.csv.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# A published image must not carry WiFi or SIP credentials. secrets.h is a
# developer override that gets compiled in, so refuse unless explicitly local.
if [[ -f "$ROOT/src/secrets.h" && " $* " != *" --allow-secrets "* ]]; then
  echo "error: src/secrets.h exists and would be baked into the image." >&2
  echo "Move it aside and rebuild, or pass --allow-secrets for a local-only test image." >&2
  exit 1
fi
ENV_NAME="esp32s3"
BUILD_DIR="$ROOT/.pio/build/$ENV_NAME"
SDKCONFIG="$ROOT/sdkconfig.$ENV_NAME"
PARTITIONS_CSV="$ROOT/partitions.csv"
OUT_DIR="$ROOT/web/firmware"
OUT_BIN="$OUT_DIR/kid_phone.bin"
MANIFEST="$ROOT/web/manifest.json"

PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
PY="${PY:-$HOME/.platformio/penv/bin/python}"
ESPTOOL="${ESPTOOL:-$HOME/.platformio/packages/tool-esptoolpy/esptool.py}"

DO_BUILD=0
VERSION=""
while [ $# -gt 0 ]; do
  case "$1" in
    --build) DO_BUILD=1 ;;
    --allow-secrets) ;;
    --version) VERSION="${2:?--version needs a value}"; shift ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

if [ "$DO_BUILD" = 1 ]; then
  echo "== building with $PIO"
  (cd "$ROOT" && "$PIO" run -e "$ENV_NAME")
fi

for f in "$BUILD_DIR/bootloader.bin" "$BUILD_DIR/partitions.bin" "$BUILD_DIR/firmware.bin" "$SDKCONFIG" "$PARTITIONS_CSV"; do
  [ -f "$f" ] || { echo "missing $f (run with --build, or build first)" >&2; exit 1; }
done
[ -f "$ESPTOOL" ] || { echo "esptool not found at $ESPTOOL (set ESPTOOL=...)" >&2; exit 1; }

# --- flash settings from the generated sdkconfig -------------------------
cfg() { sed -n "s/^$1=\"\{0,1\}\([^\"]*\)\"\{0,1\}$/\1/p" "$SDKCONFIG" | head -1; }
FLASH_MODE="$(cfg CONFIG_ESPTOOLPY_FLASHMODE)"
FLASH_FREQ="$(cfg CONFIG_ESPTOOLPY_FLASHFREQ)"
FLASH_SIZE="$(cfg CONFIG_ESPTOOLPY_FLASHSIZE)"
BOOT_OFF="$(cfg CONFIG_BOOTLOADER_OFFSET_IN_FLASH)"
PT_OFF="$(cfg CONFIG_PARTITION_TABLE_OFFSET)"
: "${FLASH_MODE:=dio}" "${FLASH_FREQ:=80m}" "${FLASH_SIZE:=16MB}" "${BOOT_OFF:=0x0}" "${PT_OFF:=0x8000}"

# --- offsets from partitions.csv -----------------------------------------
# Strip comments and spaces, then pick the first app partition (factory or
# ota_0, whichever comes first, which is what the bootloader boots on a
# fresh flash) and the otadata partition if there is one.
csv_rows() { sed -e 's/#.*//' -e 's/[[:space:]]//g' "$PARTITIONS_CSV" | awk -F, 'NF>=5 && $1!=""'; }
APP_OFF="$(csv_rows | awk -F, '$2=="app" {print $4; exit}')"
OTA_OFF="$(csv_rows | awk -F, '$2=="data" && $3=="ota" {print $4; exit}')"
[ -n "$APP_OFF" ] || { echo "no app partition with an explicit offset in $PARTITIONS_CSV" >&2; exit 1; }

PARTS=("$BOOT_OFF" "$BUILD_DIR/bootloader.bin" "$PT_OFF" "$BUILD_DIR/partitions.bin")
if [ -n "$OTA_OFF" ]; then
  OTA_BIN="$BUILD_DIR/ota_data_initial.bin"
  [ -f "$OTA_BIN" ] || { echo "partitions.csv has otadata but $OTA_BIN is missing" >&2; exit 1; }
  PARTS+=("$OTA_OFF" "$OTA_BIN")
fi
PARTS+=("$APP_OFF" "$BUILD_DIR/firmware.bin")

echo "== flash: mode $FLASH_MODE, freq $FLASH_FREQ, size $FLASH_SIZE"
echo "== bootloader $BOOT_OFF, partition table $PT_OFF, otadata ${OTA_OFF:-none}, app $APP_OFF"

mkdir -p "$OUT_DIR"
"$PY" "$ESPTOOL" --chip esp32s3 merge_bin -o "$OUT_BIN" \
  --flash_mode "$FLASH_MODE" --flash_freq "$FLASH_FREQ" --flash_size "$FLASH_SIZE" \
  "${PARTS[@]}"

# --- version into the manifest -------------------------------------------
if [ -z "$VERSION" ]; then
  VERSION="$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)"
  VERSION="$(date +%Y.%m.%d)-$VERSION"
fi
if [ -f "$MANIFEST" ]; then
  "$PY" - "$MANIFEST" "$VERSION" <<'PYEOF'
import json, sys
path, version = sys.argv[1], sys.argv[2]
with open(path) as f:
    m = json.load(f)
m["version"] = version
with open(path, "w") as f:
    json.dump(m, f, indent=2)
    f.write("\n")
PYEOF
fi

SIZE="$(wc -c < "$OUT_BIN" | tr -d ' ')"
SHA="$(shasum -a 256 "$OUT_BIN" | cut -d' ' -f1)"
echo "== wrote $OUT_BIN"
echo "   size    $SIZE bytes"
echo "   sha256  $SHA"
echo "   version $VERSION (web/manifest.json)"
echo
echo "NOTE: the image contains whatever src/secrets.h held at build time."
echo "Build public images only with secrets.h emptied (WiFi via Improv)."
