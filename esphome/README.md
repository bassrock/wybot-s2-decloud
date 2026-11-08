# Replacement firmware for the dock's ESP32-C3

Goal: a local-control firmware for the DS20 dock's ESP32-C3-MINI-1 that re-emits the same
datapoint (DP) frames to the GD32 motor MCU over the existing UART, so cleaning behavior is
unchanged but control is local (no vendor cloud).

## Design (planned)
- **Transport in:** Home Assistant (ESPHome native API) and/or local BLE, replacing the
  MQTT-to-vendor-cloud path.
- **Transport out:** UART to the GD32 motor MCU, re-emitting the `AA55`-framed DP writes/queries
  documented in [`../docs/protocol.md`](../docs/protocol.md).
- **Parity:** expose start/stop, mode, return-to-dock, and status as HA entities mapped to the
  known DP ids.

## Open items before first flash
1. **UART pins + baud** between the C3 and the GD32 (being extracted from the stock iot image;
   confirm with a logic analyzer when the dock is opened).
2. **eFuse check** — `esptool get_security_info` / eFuse summary to confirm no Secure Boot v2 /
   flash encryption (stock image is plaintext, `Secure version: 0`, so this is expected to pass).
3. **Full-flash backup** (`esptool read_flash`) before writing anything — fully reversible.

Nothing here is flashed until the stock flash is backed up.
