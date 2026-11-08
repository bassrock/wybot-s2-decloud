# WyBot S2 De-Cloud

Right-to-repair reverse engineering of a **WyBot S2** cordless pool robot and its **DS20 Solar Dock**, owned by the author. Goal: keep the robot's cleaning behavior exactly as-is, but replace the laggy cloud-dependent communication path with **local control** (e.g. Home Assistant / ESPHome), with no round-trip to the vendor cloud.

> This repo documents findings and tooling only. It does **not** redistribute WyBot's proprietary firmware binaries. Those are freely downloadable, unauthenticated, from `gateway.wybotpool.com`; run `firmware/fetch.sh` to pull and MD5-verify them locally from `firmware/manifest.tsv`.

## TL;DR architecture

The system has **two brains**, and the thing on WiFi is the **dock**, not the robot:

| Board | Chip | Role | Plan |
|-------|------|------|------|
| **iot** | ESP32-C3-MINI-1 | WiFi + BLE comms gateway, cloud (MQTT/REST) client, UART bridge to the motor MCU | **REPLACE** with local firmware |
| **robot** (`WYS2P`) | GD32E103 | robot motor + navigation MCU | keep |
| **sonar_station** | GD32E103 | dock sonar / positioning MCU | keep |
| **"9" (upper MCU)** | GD32E103 | dock charging / solar MCU | keep |

The ESP32-C3 is a transparent bridge: it speaks WiFi/BLE on one side and relays **Tuya-style datapoint (DP) frames** over UART to the GD32 motor MCU on the other. Replacing only the C3 firmware preserves all cleaning logic while cutting the cloud out of the loop.

See [`docs/architecture.md`](docs/architecture.md) for the evidence behind "the dock is the WiFi brain", [`docs/protocol.md`](docs/protocol.md) for the DP command protocol, and [`docs/ota-security.md`](docs/ota-security.md) for the firmware-delivery / integrity analysis.

## Layout

```
docs/        findings (architecture, protocol, OTA/security)
firmware/    manifest.tsv (board -> URL -> md5) + fetch.sh; binaries are gitignored
scripts/     passive MQTT firmware-catalog sniffer (credentials read from env)
esphome/     custom replacement firmware for the ESP32-C3 (work in progress)
```

## Status

- [x] Identify chips and board topology
- [x] Verify the WiFi/cloud brain lives in the dock (ESP32-C3), robot MCU is GD32 bare-metal
- [x] Obtain + MD5-verify the complete stock firmware set (all 4 boards)
- [x] Map the DP command protocol (MQTT + BLE framing)
- [x] Confirm firmware images are plaintext ESP-IDF, no app-layer crypto signature (MD5+CRC32 only)
- [x] Decompile all four images (GD32 x3 + ESP32-C3); see [`docs/boards.md`](docs/boards.md)
- [ ] Extract the C3<->MCU UART pins/baud (bridge uses the UART HAL directly, no stock IDF driver call; needs an `iot_uart_task` xref pass or a logic-analyzer read on teardown)
- [ ] Draft ESPHome firmware for the ESP32-C3
- [ ] Confirm no secure-boot/flash-encryption eFuses (needs the dock opened + UART)
- [ ] Back up stock flash, then flash custom firmware

## Legal / safety

Author-owned hardware, for interoperability and repair. No vendor accounts, device identifiers, or credentials are included here. The vendor's MQTT backend uses a single shared credential for all devices and performs no per-device authorization; that finding is documented at a responsible level in `docs/ota-security.md` without the live secret, and should be reported to the vendor rather than exploited.
