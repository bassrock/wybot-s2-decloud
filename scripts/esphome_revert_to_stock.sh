#!/usr/bin/env bash
# Revert a dock that is already running ESPHome back to the STOCK WyBot image,
# over the air, via ESPHome's own OTA. No teardown.
#
# Why this works (proven from the device-side OTA backend source, see
# docs/ota-flashing.md "Reverting to stock"): this ESPHome build compiles NO
# signature check and NO downgrade/app-descriptor check, and USE_OTA_PARTITIONS
# is off. The device-side end() does only: md5 check -> esp_ota_end() ->
# esp_ota_set_boot_partition(). esp_ota_end() validates ONLY that the payload is
# a valid ESP-IDF app (magic 0xE9 + its own appended SHA256); it does not check
# project name/identity. The stock IOT_DS20 image satisfies exactly that (magic
# 0xE9, chip_id 5, appended SHA256 valid), so ESPHome OTA accepts it and the
# real dock (chip rev <= v0.99, within the stock image's max rev v0.99) boots it.
#
# Mechanism: ESPHome's uploader streams the project's built app image (for esp-idf
# builds that is build/<name>.bin, confirmed from the upload log) and sends ITS md5.
# We temporarily swap the stock image in as that payload file, run `esphome upload`,
# then restore. (`esphome upload` has no --file flag; swap-in is the supported way to
# push an arbitrary app image through ESPHome OTA.)
#
# VERIFIED on the bench 2026-11-07: a stock image transfers completely through ESPHome
# OTA (no identity/signature/project gate). The device accepts it iff esp_ota_end()'s
# image verify passes -- magic 0xE9 + appended SHA-256 + CHIP-REV. The real dock (rev
# v0.3-0.99) passes stock's max-rev v0.99; a newer bench chip (rev 1.1) is rejected at
# esp_ota_end BEFORE the boot partition is switched, so the bench stays on ESPHome.
#
# Uses `uvx esphome` (pinned-latest) deliberately: a bare `esphome` on this Mac may be a
# stale pipx version that mixes build-tree versions and breaks the precompiled header.
#
# NOTE: this is a genuine one-way-until-you-reflash step on the real dock. Once it
# boots stock, ESPHome is gone and you are back on WyBot's firmware (and its
# cloud). You would then re-take-over via the stage-1 WyBot OTA path again.
#
# Usage:
#   scripts/esphome_revert_to_stock.sh <dock-ip-or-host> <stock.bin> [esphome-yaml]
# Example:
#   scripts/esphome_revert_to_stock.sh wybot-dock.local \
#       ~/fw/IOT_DS20_C3MINI_H0_2025_06_09_V2.2.20.bin esphome/wybot-dock.yaml
#
# On the BENCH (Super Mini, chip rev 1.1): the UNPATCHED stock image is accepted
# and written by ESPHome OTA, but will NOT boot (bootloader chip-rev lock: needs
# <= v0.99). That only proves the accept/write half. Use the maxrev-patched image
# to get it to load, at which point the stock runtime hardware guard resets it
# (RTC_SW_SYS_RST). Booting stock can only be observed on the real dock.
set -euo pipefail

DEV="${1:?usage: esphome_revert_to_stock.sh <dock-ip-or-host> <stock.bin> [yaml]}"
STOCK="${2:?path to the stock IOT_DS20 .bin}"
YAML="${3:-esphome/wybot-dock.yaml}"

[ -f "$STOCK" ] || { echo "stock image not found: $STOCK" >&2; exit 1; }
[ -f "$YAML" ]  || { echo "esphome yaml not found: $YAML" >&2; exit 1; }

# Sanity-check the stock image is what esp_ota_end() will accept, before we push it.
python3 -I - "$STOCK" <<'PY'
import sys, hashlib
f = open(sys.argv[1], 'rb').read()
ok = True
if f[0] != 0xE9:
    print("  FAIL magic byte 0x%02x != 0xE9" % f[0]); ok = False
if int.from_bytes(f[12:14], 'little') != 5:
    print("  WARN chip_id %d != 5 (ESP32-C3)" % int.from_bytes(f[12:14], 'little'))
if f[23] == 1:
    if f[-32:] != hashlib.sha256(f[:-32]).digest():
        print("  FAIL appended SHA256 invalid -> esp_ota_end() will reject"); ok = False
    else:
        print("  ok  magic 0xE9, appended SHA256 valid")
else:
    print("  note image has no appended hash; esp_ota_end() checks magic only")
print("  max_chip_rev_full = v0.%d (real dock must be <= this to BOOT)"
      % int.from_bytes(f[17:19], 'little'))
sys.exit(0 if ok else 2)
PY

# The built app image ESPHome actually uploads for an esp-idf build.
BUILD_BIN=".esphome/build/wybot-dock/build/wybot-dock.bin"
if [ ! -f "$BUILD_BIN" ]; then
  echo "No built app image yet; compiling once so the build tree exists..."
  uvx esphome compile "$YAML"
fi

BAK="${BUILD_BIN}.esphome-backup"
cp -f "$BUILD_BIN" "$BAK"
restore() { cp -f "$BAK" "$BUILD_BIN" && rm -f "$BAK"; echo "restored ESPHome payload"; }
trap restore EXIT

echo ">> swapping stock image in as the OTA payload and pushing to $DEV"
cp -f "$STOCK" "$BUILD_BIN"
uvx esphome upload "$YAML" --device "$DEV"
echo ">> upload finished; on a chip-compatible device it reboots into the stock image"
