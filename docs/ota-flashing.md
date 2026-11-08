# Flashing custom firmware over the air (no teardown)

Goal: get ESPHome onto the dock's ESP32-C3 **without opening the case**, then use ESPHome's own
OTA for everything after. This is a two-stage process.

## Stage 1 — the one-time takeover (rides in on WyBot's OTA channel)
The stock firmware is what's running, so the first custom flash must use the dock's existing OTA
path. WyBot's OTA (`/device/OTA/recv_start_update/{dockId}` over its MQTT) hands the device a
download URL + md5; the device fetches it over plain HTTP from `gateway.wybotpool.com` and
`esp_ota`-writes it to the inactive app slot.

What the decompiled stock iot image tells us about that handler:
- **No cryptographic signature** on images, and **no app-descriptor / project-name / version check**
  before flashing. It md5s the download and writes it. => an ESPHome image (different project name)
  is accepted. (Uses `esp_ota_get_next_update_partition` + `esp_ota_ops.c`: standard IDF dual-OTA.)
- **No app-rollback.** No `esp_ota_mark_app_valid` / `pending_verify` / `cancel_rollback` symbols are
  present, i.e. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is off. **There is no automatic revert.**

### Consequence / safety
Because the device accepts unsigned images, Secure Boot is effectively ruled out (a Secure-Boot
device could not boot WyBot's own unsigned OTA images), so an unsigned ESPHome image should boot.
But with no rollback, **if the first image fails to boot, the dock is dead until it is opened and
reflashed over UART.** Therefore stage 1 must use a *known-good* image:

1. Build `esphome/wybot-dock.yaml` → app image (`firmware.bin`, the OTA image, not the factory image).
2. **Pre-validate it:** flash that exact binary to a spare ESP32-C3-MINI-1 over USB and confirm it
   boots and joins WiFi. (A fallback AP + captive portal is enabled so a WiFi mistake is recoverable.)
3. Host the validated `firmware.bin` on a local HTTP server; compute its md5.
4. Publish a forged `recv_start_update` for the dock pointing at that URL + md5 (schema reversed from
   the stock OTA handler; `boardID` targets the iot board). The dock downloads, flashes the inactive
   slot, switches boot, and reboots into ESPHome.

### Residual risks
- **Partition fit:** ESPHome app (~1.1 MB) must fit the dock's OTA slot. Stock app is ~1.48 MB in a
  4 MB flash, so a standard dual-OTA slot (~1.9 MB) fits comfortably.
- **Partition-table differences:** the device keeps WyBot's partition table (we replace only the app).
  IDF reads the real table from flash at runtime, so ESPHome adapts; booting depends only on the
  bootloader loading the app segments (table-independent). Lowest-risk, but the reason pre-validation
  can't be 100% identical (the spare module uses ESPHome's own table).
- **Chip revision (checked, OK):** the stock image is built with max chip rev v0.99, so the dock's C3
  is rev v0.3–v0.99. Our ESPHome image accepts v0.3–v1.99, which contains the dock's rev, so the
  bootloader won't reject it on a revision check. (See `bench-validation.md`.)

### The `recv_start_update` message (schema)
Topic: `/device/OTA/recv_start_update/{dockId}` (published app/cloud → device).

Field schema is **known with high confidence** from the live `reply_all_firmware_info` catalog
(same fields the cloud serves) and from the device's own cJSON key strings confirmed in the stock
image (`boardID`, `url`, `softwareVer`, `hardwareVer`, `productModel`, `chipType`, `progress`, plus
`md5` used by WyBot's own `iot_md5.c` verifier). A firmware entry looks like:

```json
{ "id":<int>, "boardID":"iot", "productModel":"DS20", "chipType":"",
  "softwareVer":"2.2.20", "hardwareVer":"5.127.0",
  "url":"http://<your-host>/firmware.ota.bin", "md5":"<hex>",
  "target":"0.0.0", "dependency":"", "enforce":false, "manual":1 }
```

To flash our image we set `boardID":"iot"` (the C3), point `url` at our locally-hosted
`firmware.ota.bin`, and set `md5` to its md5. The device downloads over plain HTTP, md5-verifies,
and `esp_ota`-writes it.

**One unknown — the envelope.** Whether the payload is the bare entry object, `{"firmware":[entry]}`,
or a `{"ts":..,"cmd":..}`-wrapped form (as the `/device/DATA/*` messages use) is **not** resolved
from the stripped RISC-V image (all OTA topic strings are `auipc`/`addi` PC-relative, which Ghidra
doesn't recover on a raw import). This is confirmed cheaply by **one passive capture** of a real
`recv_start_update` (see `scripts/ota_capture.py`) rather than deeper binary reversing. `scripts/ota_push.py`
supports all three envelope forms via `--envelope`.

## Stage 2 — ESPHome OTA forever after
Once ESPHome is running, update it from the ESPHome dashboard / `esphome run` like any ESP device.
No cloud, no teardown, no WyBot OTA. This is the end state "flash OTA via ESPHome."

## Then: restore full function (still no teardown)
ESPHome, now on the C3, can scan its own GPIOs to find which pins carry the UART to the GD32 motor
MCU (watch for 3.3 V UART idle / traffic), then the DP bridge component is enabled and the dock
drives the robot locally. See `esphome/wybot-dock.yaml` and `protocol.md`.

## Alternative stage-1 channel
WyBot also has a **BLE OTA** path (`_BtOtaDockerUpgrade`, app streams the image in chunks). Same
no-signature/no-rollback properties; more protocol to reverse than the MQTT path, so MQTT is the
default. Kept as a fallback if the MQTT OTA proves awkward to trigger.

## Reverting to stock (ESPHome OTA, no teardown) — the takeover is reversible

Yes: once ESPHome is on the dock, ESPHome's **own** OTA can flash the **stock
WyBot image** back, over the air, with no teardown. Verified from the device-side
OTA backend compiled into our build (`ota_backend_esp_idf.cpp`), not assumed:

- This build defines **`USE_OTA`, `USE_OTA_DEFLATE`, `USE_OTA_ENCRYPTION`,
  `USE_OTA_PASSWORD`, `USE_OTA_ROLLBACK`, `USE_OTA_VERSION`** — and **not**
  `USE_OTA_SIGNED_VERIFICATION*`, **not** `USE_OTA_DOWNGRADE_PROTECTION`, **not**
  `USE_OTA_PARTITIONS`.
- So `IDFOTABackend::end()` reduces to: optional **md5** check → **`esp_ota_end()`**
  → **`esp_ota_set_boot_partition()`**. No project-name / identity / version gate.
- `esp_ota_end()` validates only that the payload is a valid ESP-IDF **app**
  (header magic `0xE9` + the image's own appended SHA-256). It does **not** care
  that the app isn't ESPHome.

The stock `IOT_DS20_C3MINI...V2.2.20.bin` satisfies exactly that check: magic
`0xE9`, `chip_id = 5` (ESP32-C3), `hash_appended = 1` with a **valid** appended
SHA-256, `max_chip_rev_full = v0.99`. So on the **real dock** (chip rev between
v0.3 and v0.99) ESPHome OTA accepts it, writes it to the inactive slot, flips the
boot partition, and the dock boots **stock** — fully reversible, air-gapped.

Do it with `scripts/esphome_revert_to_stock.sh <dock-ip> <stock.bin>`: it swaps
the stock image in as ESPHome's OTA payload (`esphome upload` has no `--file`
flag) and pushes it. Reverting drops you back onto WyBot's firmware + cloud; you
re-take-over via the stage-1 WyBot OTA path again if wanted. Keep `USE_OTA_ROLLBACK`
(present) so a bad flash is recoverable, and keep the stock `.bin` (md5
`422b804420b90ce6cbe5b96eab588887`) as the known-good revert target.

### Why the full round-trip can't be *booted* on the bench
On the Super Mini (chip rev **1.1**) ESPHome OTA still **accepts and writes** the
stock image (same `end()` path), but the UNPATCHED stock won't **boot** — the
bootloader chip-rev lock requires `<= v0.99`. The maxrev-patched image boots far
enough to hit the stock runtime hardware guard (`RTC_SW_SYS_RST` loop, see
`bench-validation.md`). So the bench proves the **accept/write** half; **booting
stock** can only be observed on the real dock. The accept/write half itself is now
proven from source above, so a live bench write adds nothing the source doesn't
already establish — the only thing a bench run needs that we don't have is real
WiFi creds in `esphome/secrets.yaml` (ESPHome OTA is network-only; the Mac can't
join the dock's fallback AP here without dropping its own default route).
