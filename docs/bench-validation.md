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

## Live bench run (2026-11-07): WiFi join + ESPHome OTA, both directions

With real creds in `secrets.yaml`, the Super Mini was flashed over USB and:

- **Joined WiFi, but only after an RF fix.** First boot looped on `reason='Auth Expired'`
  against a plain WPA2/CCMP 2.4 GHz SSID (verified from the UDM: `wpa3_support:false`,
  `pmf_mode:disabled`, `mac_filter_enabled:false` — none of which would block it). Root
  cause is the ESP32-C3 "Super Mini" clone browning out / distorting TX at the default
  20 dBm on its PCB antenna, which corrupts the WPA2 handshake. Fix (same as Daniel's
  water-softener C3): **`wifi: output_power: 8.5dB`**. After that it associated cleanly —
  UniFi shows it online, authorized, satisfaction 100%, ~-71 dBm. The real dock's module
  may tolerate more power, but 8.5 dB is the known-good value for this board.
- **Forward ESPHome OTA over WiFi works.** `esphome upload --device <ip>` → "OTA
  successful" in ~11 s. This is the end-state "flash OTA via ESPHome."
- **Stock-revert accept test (unpatched stock, over ESPHome OTA).** The stock
  `IOT_DS20` image **transferred completely** through ESPHome OTA (full 1.48 MB, 948 KB
  deflated) — no project-name / identity / signature gate rejected it, corroborating the
  source analysis in `ota-flashing.md`. It failed only at the final `esp_ota_end()` step,
  on the **chip-revision** check: `esp_ota_end()` runs `esp_image_verify`, which rejects
  an image whose `max_chip_rev` (stock = v0.99) is below the actual chip (bench = v1.1).
  That check fires **before** `set_boot_partition`, so the device never switched and kept
  running ESPHome (verified still reachable after). 

  Implication for the real dock: its chip is rev v0.3–v0.99, so the stock image's
  v0.99 max is satisfied and the same `esp_ota_end()` succeeds → `set_boot_partition` →
  boots stock. I.e. the only gate ESPHome OTA applies to a stock image is `esp_ota_end`'s
  image validity (magic + appended SHA-256 + chip-rev) — never identity or signature — and
  the real dock passes all three. To force the commit on the bench instead you must use the
  **maxrev-patched** stock image; that then boots far enough to hit the stock runtime
  hardware guard (reset loop) and needs a USB reflash to recover — which is why the
  unpatched image is the right, self-recovering bench probe.
