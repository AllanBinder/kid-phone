#!/bin/sh
# Build and merge bootloader + partition table + app into one image at offset 0
# for the browser flasher (web/). Run from the repo root.
set -e
PIO=~/.platformio/penv/bin
B=.pio/build/esp32s3
$PIO/pio run
$PIO/python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 merge_bin \
  -o web/kid_phone.bin \
  0x0 $B/bootloader.bin 0x8000 $B/partitions.bin 0x10000 $B/firmware.bin
echo "web/kid_phone.bin ready"
