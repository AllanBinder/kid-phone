# PlatformIO pre-build script: teach the ESP-IDF 5.5.3 bootloader about XMC
# D-series flash (JEDEC vendor byte 0x46) and XMC 0x2070xx chips (see the
# second check at the end).
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

# Second check (0.8.1, GitHub issue #3): XMC chips with RDID 0x2070xx, seen on
# an N16R8 clone (RDID 20 70 18, SFDP vendor 0x20, 16 MB). They work normally
# (esptool reads and writes them), but is_xmc_chip_strict() only knows mfid
# 0x40/0x41/0x50, so the bootloader runs the startup flow, still finds an
# unknown model and reboot-loops with "XMC flash startup fail". Accept mfid
# 0x70 as a known model (2 MB to 32 MB, cpid 0x15 to 0x19), which skips the
# flow. Patch contributed by jordimarcel.
ANCHOR_70 = """    } else if (mfid == 0x50) {
        if (cpid >= 0x15 && cpid <= 0x16) {
            matched =  true;
        }
    }
"""
PATCHED_70 = """    } else if (mfid == 0x50) {
        if (cpid >= 0x15 && cpid <= 0x16) {
            matched =  true;
        }
    } else if (mfid == 0x70) {
        // kid_phone: XMC 0x2070xx seen on N16R8 clones, works without the flow
        if (cpid >= 0x15 && cpid <= 0x19) {
            matched = true;
        }
    }
"""

with open(SRC, encoding="utf-8") as f:
    text = f.read()

if "mfid == 0x70" in text:
    print("xmc patch: bootloader already accepts XMC 0x2070xx")
elif text.count(ANCHOR_70) == 1:
    with open(SRC, "w", encoding="utf-8") as f:
        f.write(text.replace(ANCHOR_70, PATCHED_70))
    print("xmc patch: 0x2070xx accepted in " + SRC)
else:
    env.Exit("xmc patch: mfid 0x50 branch not found in " + SRC +  # noqa: F821
             "; check the XMC 0x2070xx fix by hand")
