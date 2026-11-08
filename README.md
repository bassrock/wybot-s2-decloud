# WyBot S2 De-Cloud

Right-to-repair reverse engineering of a **WyBot S2** cordless pool robot and its **DS20 Solar Dock**, owned by the author. Goal: keep the robot's cleaning behavior exactly as-is, but replace the laggy cloud-dependent communication path with **local control** (Home Assistant / ESPHome), with no round-trip to the vendor cloud.

Only the dock's **ESP32-C3** comms firmware is replaced. The GD32 motor/navigation MCU is never touched, so all cleaning logic is preserved; the cloud is simply cut out of the loop.

> This repo documents findings and tooling only. It does **not** redistribute WyBot's proprietary firmware binaries. Those are freely downloadable, unauthenticated, from `gateway.wybotpool.com`; run `firmware/fetch.sh` to pull and MD5-verify them locally from `firmware/manifest.tsv`.

## ⚠️ Disclaimer — no warranty, use at your own risk

**THIS IS PROVIDED "AS IS", WITH NO WARRANTY OF ANY KIND AND NO LIABILITY.** To the maximum extent permitted by law, the author(s) and contributors are **not liable** for any damage, loss, injury, or cost arising from use of this material — including a **bricked dock, a damaged or lost robot, property damage, voided warranty, or a violated terms-of-service**.

Specifically, understand before you do anything here:

- **The first custom flash is irreversible and can brick your dock.** The stock bootloader has **no rollback**. If the image you flash does not boot, the dock is dead until you open it and reflash over UART. Pre-validate the exact image on a spare ESP32-C3 first.
- **A pool robot is a powered machine operating in water.** Local control means *you* are now responsible for its behavior. Do not send it commands you can't physically supervise.
- **This modifies vendor firmware and uses the vendor's OTA/MQTT channel.** That may **void your warranty** and may violate the vendor's terms of service. That is your decision to make about **your own** hardware.
- Nothing here is endorsed by, affiliated with, or supported by WyBot. "WyBot" and product names are used only to identify the hardware.

This is interoperability/repair work on hardware the author owns. If you proceed, you accept all risk.

## TL;DR architecture

The system has **two brains**, and the thing on WiFi is the **dock**, not the robot:

| Board | Chip | Role | Plan |
|-------|------|------|------|
| **iot** | ESP32-C3-MINI-1 | WiFi + BLE comms gateway, cloud (MQTT/REST) client, UART bridge to the motor MCU | **REPLACE** with ESPHome |
| **robot** (`WY450`) | GD32E103 | robot motor + navigation MCU | keep |
| **sonar_station** | GD32E103 | dock sonar / positioning MCU | keep |
| **"9" (upper MCU)** | GD32E103 | dock charging / solar MCU | keep |

The ESP32-C3 is a transparent bridge: it speaks WiFi/BLE on one side and relays **Tuya-style datapoint (DP) frames** over UART to the GD32 motor MCU on the other. Replacing only the C3 firmware preserves all cleaning logic while cutting the cloud out of the loop.

The **confirmed** physical link and DP protocol are in [`docs/protocol-confirmed.md`](docs/protocol-confirmed.md) (verified live on the real dock): **UART0, 115200 8N1, GPIO21 = C3→MCU, GPIO20 = MCU→C3**, frame `AA 55 00 | cmd | len(LE) | payload | checksum`. (The older [`docs/protocol.md`](docs/protocol.md) and [`docs/uart-pins.md`](docs/uart-pins.md) are kept as superseded notes — they guessed UART1, which was wrong.)

See [`docs/architecture.md`](docs/architecture.md) for the evidence that the dock is the WiFi brain, and [`docs/ota-security.md`](docs/ota-security.md) for the firmware-delivery / integrity analysis.

## Layout

```
docs/          findings (architecture, confirmed protocol, OTA/security, bench validation)
firmware/      manifest.tsv (board -> URL -> md5) + fetch.sh; binaries are gitignored
scripts/       flash_from_stock.py (the one-shot takeover) + lower-level OTA/MQTT tools + revert
esphome/       replacement firmware for the ESP32-C3: the wybot_bridge component + device YAMLs
homeassistant/ the template-vacuum package that wraps the ESPHome entities into a vacuum entity
```

## How to take over a stock dock (over the air, no teardown)

Full detail and the safety reasoning are in [`docs/ota-flashing.md`](docs/ota-flashing.md). The short version:

1. **Build the replacement image.** `uvx esphome compile esphome/wybot-dock-bridge.yaml` (fill in `esphome/secrets.yaml` from `secrets.example.yaml` first — it is gitignored).
2. **Pre-validate it on a spare ESP32-C3-MINI-1 over USB.** Confirm it boots and joins WiFi. The stock bootloader has no rollback, so this step is what stands between you and a brick.
3. **Flash the real dock, over the air:**
   ```bash
   export WYBOT_MQTT_HOST=mqtt.wybotpool.com WYBOT_MQTT_USER=... WYBOT_MQTT_PASS=...
   python3 scripts/flash_from_stock.py --device <dockId> --host-ip <your-LAN-ip> --dry-run   # inspect
   python3 scripts/flash_from_stock.py --device <dockId> --host-ip <your-LAN-ip> --yes        # arm
   ```
   The script validates the image header (magic / chip-id / appended SHA-256) and refuses to flash a non-bootable one, serves it over HTTP, publishes the firmware-offer that triggers the dock's own OTA, and streams progress. It uses the vendor's **single shared** MQTT credential (not a per-user secret), supplied via env.
4. **From then on, update via ESPHome OTA** (`esphome run esphome/wybot-dock-bridge.yaml`). No cloud, no teardown.
5. **Reverting to stock** is also OTA and non-destructive: `scripts/esphome_revert_to_stock.sh <dock-ip> <stock.bin>` (see `docs/ota-flashing.md`).

## Home Assistant

`esphome/wybot-dock-bridge.yaml` exposes the dock as a local ESPHome device: Start/Stop/Return buttons, a Cleaning-Mode select, robot/solar battery + status sensors, and an on-device **Cleaning** binary sensor (derived from battery-drain + a run-counter, because the dock's own status datapoint under-reports once the robot swims away from the dock).

`homeassistant/packages/wybot.yaml` wraps those entities into a single `vacuum.wybot_pool_robot` (a HA *modern* template vacuum — ESPHome has no vacuum entity type, so the vacuum lives in HA; the firmware provides the controls and telemetry it drives).

## Status

- [x] Identify chips and board topology; confirm the WiFi/cloud brain is the dock's ESP32-C3
- [x] Obtain + MD5-verify the complete stock firmware set (all 4 boards)
- [x] Confirm stock images are plaintext ESP-IDF, **no app-layer crypto signature** (MD5+CRC32 only), no rollback
- [x] Decompile all four images (GD32 x3 + ESP32-C3); see [`docs/boards.md`](docs/boards.md)
- [x] Bench-validate on a spare C3: image boots, DP-bridge compiles+runs, chip-rev compatible ([`docs/bench-validation.md`](docs/bench-validation.md))
- [x] Rehearse stage-2 ESPHome OTA on the bench (needs `output_power: 8.5dB` for Super Mini clones)
- [x] **Flash the real dock over the air** via the firmware-offer path (`scripts/flash_from_stock.py`)
- [x] **Crack the C3↔MCU link live:** UART0 115200 GPIO21/20; full `AA5500` DP frame + write command ([`docs/protocol-confirmed.md`](docs/protocol-confirmed.md))
- [x] **ESPHome bridge** drives the robot locally: Start/Stop/Return, mode select, battery/status, on-device cleaning detection
- [x] **Home Assistant** integration: local vacuum entity + example automations
- [x] Confirm the takeover is **reversible**: ESPHome OTA flashes the stock image back ([`docs/ota-flashing.md`](docs/ota-flashing.md#reverting-to-stock-esphome-ota-no-teardown--the-takeover-is-reversible))

## Legal / responsible disclosure

Author-owned hardware, for interoperability and repair. No vendor accounts, device identifiers, home-network identifiers, or personal credentials are included in this repo. The vendor's MQTT backend uses a **single shared credential for all devices** and performs no per-device authorization; that finding is documented at a responsible level in `docs/ota-security.md` and should be reported to the vendor rather than exploited. (The shared credential is not a user secret, which is why env-based use is documented here; it is still the vendor's to fix.)
