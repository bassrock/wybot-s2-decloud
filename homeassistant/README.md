# Home Assistant integration

Once the dock runs `esphome/wybot-dock-bridge.yaml`, HA auto-discovers it as an ESPHome device
("WyBot Dock") exposing buttons (Start / Stop / Return to Dock), a Cleaning-Mode select, battery
and status sensors, and an on-device **Cleaning** binary sensor.

## Vacuum entity

ESPHome has no `vacuum` entity type, so the vacuum lives in HA as a **modern template vacuum** that
wraps the ESPHome entities. Add it via a package:

1. In `configuration.yaml`, under `homeassistant:`, add `packages: !include_dir_named packages`.
2. Copy [`packages/wybot.yaml`](packages/wybot.yaml) to `<config>/packages/wybot.yaml`.
3. Restart HA (a legacy-platform vacuum needs a full restart the first time).

You get `vacuum.wybot_pool_robot` with start / stop / return-to-base and fan-speed = cleaning mode.

## Example automations

[`automations-examples.yaml`](automations-examples.yaml) has two reusable automations — a scheduled
"Clean Pool" (only from a full charge) and a "Low Battery Return Home". Both are deliberately
anchored on the **battery** and **charging** signals rather than the dock's `docked` / status
datapoints, which flicker and go stale once the robot swims away from the dock (see
[`../docs/protocol-confirmed.md`](../docs/protocol-confirmed.md)).

> Heads-up from real use: a pool robot that can't physically re-seat on the dock will keep retrying
> and drain itself. Keep an eye on the first few low-battery returns, and disable the return
> automation if the robot is out of the water or being serviced.
