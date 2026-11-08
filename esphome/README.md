# Replacement firmware for the dock's ESP32-C3

Local-control ESPHome firmware for the DS20 dock's ESP32-C3-MINI-1. It re-emits the same
datapoint (DP) frames to the GD32 motor MCU over the existing UART, so cleaning behavior is
unchanged but control is local (Home Assistant native API, no vendor cloud).

**Status: working on a real dock.** The C3↔MCU link is confirmed as **UART0, 115200 8N1,
`tx_pin: GPIO21` (C3→MCU), `rx_pin: GPIO20` (C3←MCU)**. The full `AA5500` DP frame and the write
command are documented in [`../docs/protocol-confirmed.md`](../docs/protocol-confirmed.md).

## Files

| File | What it is |
|------|------------|
| `wybot-dock-bridge.yaml` | **The functional image.** UART bridge + all DP sensors, the `select`/`button` controls, and the on-device cleaning-detection binary sensor. This is what runs on the dock. |
| `wybot-dock.yaml` | Connectivity-only image (WiFi + API + OTA, no GPIO). A known-good minimal boot used to prove the board joins WiFi; kept as a safe fallback. |
| `wybot-dock-pinscan.yaml` | One-shot UART pin scanner (`wybot_pinscan` component) used to find GPIO21/20. Kept for reproducibility. |
| `wybot-dock-tap.yaml` | Passive UART tap used to capture live MCU frames. |
| `components/wybot_bridge/` | The reusable external component (C3↔MCU DP bridge). |
| `components/wybot_pinscan/` | The pin-scanner external component. |

All four YAMLs use `name: wybot-dock`, so every one of them compiles to
`.esphome/build/wybot-dock/build/firmware.ota.bin` (the path `scripts/flash_from_stock.py` expects).

## Using the `wybot_bridge` component

### From this repo, over the network (no files to copy)

The component is served straight from the repo. Point ESPHome at the GitHub source:

```yaml
external_components:
  - source: github://bassrock/wybot-s2-decloud@main
    components: [ wybot_bridge ]

uart:
  id: mcu
  tx_pin: GPIO21
  rx_pin: GPIO20
  baud_rate: 115200

wybot_bridge:
  id: bridge
  uart_id: mcu
  cleaning:
    name: "Cleaning"
```

Pin a tag/commit instead of `@main` for reproducible builds (e.g. `...decloud@v1`).

### From a local checkout

If you cloned the repo and are editing the component, use the local path (what the example YAMLs
in this folder do):

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [ wybot_bridge ]
```

### Component options

```yaml
wybot_bridge:
  id: bridge
  uart_id: mcu
  heartbeat_interval: 2s      # how often to poke the MCU so it keeps reporting
  cleaning:                   # optional: on-device "actually cleaning" binary sensor
    name: "Cleaning"
  battery_datapoint: 50       # DP whose byte[1] is robot battery %, byte[0] is charge state
  activity_datapoint: 111     # fast run-counter DP used as the activity proxy
  activity_threshold: 16      # counter advance/poll that counts as "moving"
  cleaning_timeout: 6min      # latch-off if no activity and not charging
  numeric_datapoints: [ ... ] # map DPs (or byte slices) to sensors
  text_datapoints:   [ ... ]  # map DPs to hex/int text sensors
```

Cleaning detection does not trust DP0 status (it goes stale once the robot leaves the dock). It
latches **cleaning = true** on battery drain or a fast-rising `activity_datapoint`, and latches it
**false** on charge-state or the inactivity timeout. See
[`../docs/protocol-confirmed.md`](../docs/protocol-confirmed.md) for the DP map.

## Flashing

- **First takeover from stock (irreversible, no bootloader rollback):**
  `scripts/flash_from_stock.py` — read its header first and pre-validate the exact image on a spare
  ESP32-C3 over USB. Super Mini clones need `output_power: 8.5dB` to join WiFi.
- **After that:** ordinary ESPHome OTA (`uvx esphome run esphome/wybot-dock-bridge.yaml`).
- **Back to stock:** `scripts/esphome_revert_to_stock.sh`.

`secrets.yaml` is gitignored; copy `secrets.example.yaml` to `secrets.yaml` and fill in your own
WiFi / API / OTA values.
