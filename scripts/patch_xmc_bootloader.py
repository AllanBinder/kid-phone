# PlatformIO pre-build script: teach the ESP-IDF 5.5.3 bootloader about XMC
# D-series flash (JEDEC vendor byte 0x46).
#
# Why: some ESP32-S3 N16R8 clone boards ship with an XMC D-series flash chip.
# Its SFDP table reports XMC (0x20), so the 5.5.3 bootloader runs the
# XM25QHxxC startup flow, then rejects the chip because 0x46 is missing from
# is_xmc_chip_strict(). Result: "XMC flash startup fail" boot loop on a fresh
# install. Espressif fixed this upstream (ESP-IDF master) with the same check
# added here. D-series chips do not need the startup flow.
#
# Turning CONFIG_BOOTLOADER_FLASH_XMC_SUPPORT off is NOT the fix: that branch
# rejects every 0x20 chip, which would brick boards with older XMC flash.
#
# The patch edits the framework package in place (PlatformIO builds the
# bootloader straight from it). It is idempotent, and a no-op once the
# framework already carries the upstream fix. Fails the build if the source
# no longer looks the way this script expects.
import os

Import("env")  # noqa: F821 (provided by PlatformIO)

FRAMEWORK_DIR = env.PioPlatform().get_package_dir("framework-espidf")  # noqa: F821
SRC = os.path.join(FRAMEWORK_DIR, "components", "bootloader_support",
                   "bootloader_flash", "src", "bootloader_flash.c")

ANCHOR = """    uint32_t cpid = BYTESHIFT(rdid, 0);

    if (vendor_id != XMC_VENDOR_ID_1) {
        return false;
    }
"""
PATCHED = """    uint32_t cpid = BYTESHIFT(rdid, 0);

    // xmc vendor id begin with 0x46 means that it's a D-series chip
    // which did not need the startup flow
    if (vendor_id == 0x46) {
        return true;
    }

    if (vendor_id != XMC_VENDOR_ID_1) {
        return false;
    }
"""

with open(SRC, encoding="utf-8") as f:
    text = f.read()

if "vendor_id == 0x46" in text:
    print("xmc patch: bootloader already accepts XMC D-series (0x46)")
elif text.count(ANCHOR) == 1:
    with open(SRC, "w", encoding="utf-8") as f:
        f.write(text.replace(ANCHOR, PATCHED))
    print("xmc patch: applied to " + SRC)
else:
    env.Exit("xmc patch: is_xmc_chip_strict() not found in " + SRC +  # noqa: F821
             "; check the XMC D-series fix by hand before releasing an image")
