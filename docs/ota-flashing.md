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
