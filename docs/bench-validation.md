# Bench validation on a spare ESP32-C3 (Super Mini)

A spare ESP32-C3 "Super Mini" (same chip + 4 MB flash as the dock) is used to prove things
before any OTA touches the real dock. The dock itself is **never** wired up; it will only ever
be flashed over the air.

## Confirmed
- **Our ESPHome image boots on real C3 hardware.** Both the minimal image and the full dock
  firmware (WiFi + native API + ESPHome-OTA + captive-portal fallback + the UART DP bridge +
  HA actions) flash over USB, boot cleanly, run stably (no reset loop, no panic), and bring up
  WiFi (failing only because the bench `secrets.yaml` still has placeholder credentials).
- **The DP-bridge firmware compiles and runs.** `esphome/components/wybot_bridge` builds into
  the image (Flash ~49%, ~906 KB — still well under the dock's 1.79 MB slot).
- **Chip-revision compatibility (important).** The stock DS20 image is built with **max chip
  revision v0.99**, so the real dock's C3 is an *older* revision (between v0.3 and v0.99). Our
  ESPHome image's accepted window is **v0.3 → v1.99**, which contains the dock's revision, so the
  dock's bootloader will accept our image over OTA (no chip-rev rejection).

## Why the real stock firmware can't be rehearsed on the bench
We tried running the genuine stock `IOT_DS20` firmware on the Super Mini (wrapped it into a
bootable layout at app0 with a bootloader + partition table). Two walls:
1. **Chip-rev lock.** The newer Super Mini (rev 1.1) exceeds the stock image's max rev (v0.99),
   so the bootloader refused it (`chip revision check failed. Required <= v0.99, found v1.1`).
   This was defeated by patching the image header's `max_chip_rev` to 0xFFFF and recomputing the
   appended SHA-256 — which the device accepts, re-confirming **there is no image signature**.
2. **Runtime hardware guard.** Once loaded, the stock app does an immediate, clean software
   reset (`RTC_SW_SYS_RST`) with no output — a deliberate early guard that bails when it doesn't
   find the dock's hardware/NVS state. So stock won't run on a bare board, and a faithful
   stock→ESPHome takeover rehearsal isn't possible on the Super Mini.

Consequence: the WyBot-specific OTA handler behaviour (exact `recv_start_update` envelope,
whether it flashes our image) can only be validated on the **real dock** — via a passive
`ota_capture.py` first, then the real `ota_push.py`. The bench still de-risks the big things:
our image boots on C3, fits, is chip-rev compatible, and the whole ESPHome stage-2 OTA loop can
be rehearsed on the bench once it is on WiFi.

## Still to do on the bench (needs real WiFi creds in `esphome/secrets.yaml`)
- Confirm WiFi join + HA native API adoption.
- **Rehearse stage-2:** do a real ESPHome OTA update to the Super Mini over the network (this is
  the end-state "flash OTA via ESPHome").
- Optionally rehearse `ota_push.py` end-to-end against a small local stand-in that mimics the
  WyBot MQTT OTA (connect to a local broker → download our URL → `esp_ota` → boot), to exercise
  the push/host/flash pipeline (not WyBot's exact handler).
