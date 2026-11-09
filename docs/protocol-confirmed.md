# WyBot DS20 dock <-> GD32 MCU protocol — CONFIRMED (live)

Recovered by static RE of the stock `IOT_DS20` C3 image and **confirmed on the real dock** on
2026-11-07. This supersedes the guesses in `protocol.md` / `uart-pins.md` (which assumed UART1 and
an unverified framing).

## Physical link
- **UART0**, **115200 baud, 8N1**.
- **GPIO21** = C3 TX -> MCU RX. **GPIO20** = C3 RX <- MCU TX.
- Console/log is on USB-Serial-JTAG, so UART0 is free for the MCU. (Stock proof: the only
  `uart_set_pin` call in the image is `uart_set_pin(0, 0x15, 0x14, -1, -1)`; param struct baud
  `0x1C200` = 115200. FreeRTOS task "com" services it.)
- The MCU is a **slave**: it idles TX high and only answers when polled. Passive listening sees
  nothing; you must heartbeat + poll.

## Frame
```
AA 55 00 | cmd(1) | plen(2, LITTLE-endian) | payload[plen] | cksum(1)
cksum = (sum of every byte from the first AA through the last payload byte) & 0xFF
```
- `cmd` C3->MCU: `0x00` heartbeat, `0x09` query DP (payload = one dp_id), `0x04` write DP.
- `cmd` MCU->C3: `0x00` heartbeat reply (const payload `10 10 00`), `0x05` DP report.
- Unknown commands are ignored (no echo). `0x02`/`0x10` return an empty-payload ack.
- The stock image also builds two other framings for other targets (`A5 …` and `55 AA 02 …`);
  the live dock MCU speaks the `AA 55 00` one.

## Datapoint (DP) encoding
A `0x05` report payload is one or more DPs concatenated:
```
dp_id(1) | dp_type(1) | dp_len(1) | data[dp_len]
```
- types: `0x01` bool(1B), `0x02` int (dp_len bytes, **little-endian, signed**), `0x04` enum(1B),
  `0x05` bitmap(4B), `0x00` raw.
- **Query** a DP: `cmd 0x09`, payload = `dp_id`. MCU answers with a `0x05` report for that DP.
- **Write** a DP: `cmd 0x04`, payload = `dp_id, dp_type, dp_len, data…` (same encoding as a report
  entry). Confirmed live: writing DP1=6 returned a `05` report echoing `01 04 01 06`.

## DP map (this unit = S2 Pro robot + DS20 solar dock; authoritative = pywybot, APK-confirmed)
| DP | meaning | notes |
|----|---------|-------|
| 0  | cleaning status | 1 stopped / 2 returning / 3 cleaning / 4 returning-to-dock / 255 starting. **Write 03 start, 01 stop** |
| 1  | cleaning mode | enum 0..6: Floor, Wall, Wall Then Floor, Advanced Full Pool, Water Line, Turbo Floor, Eco Floor |
| 11 | return to dock | **Write 01** |
| 50 | battery | 2-byte `[charge_state][robot_batt%]`; charge_state 0 unplugged / 1 charging / 2 charged |
| 79 | schedule | 12 bytes |
| 209| device status | enum |
| 212| connected | 1 = connected |
| 213| docked | 1 = docked |
| 214| dock type | 5 = Solar (confirmed) |
| 221| solar dock battery | 3-byte `[flags][batt%][?]`; byte1 per pywybot. Live `01000A` reads 0% — verify byte in daylight |
| 131| working time (s) | LE4 — **absent on this unit** |
| 222| solar charging | **absent on this unit** |

F1-skimmer-only DPs (not this model): 70 systime, 142 pH+temp, 145 heavy-dirt, 206 depth, 207 auto-run.

57 DPs total respond to a 0..255 query sweep. As of 2026-11-09 **every responding DP is captured**
in the firmware (no silent drops): the load-bearing ones carry real entities, and the rest are
bare `DP<n>` diagnostic sensors. A catch-all in the component also WARN-logs (once) any reported DP
that still has no entity, so a newly-appearing DP can never go unnoticed. The bare ones are internal
telemetry the cloud app never surfaced — decode them by correlating against a real cleaning/charge
cycle, then add a properly named entity and retire the `DP<n>` one (renaming orphans HA history).

### Poll tiering (added 2026-11-09, Fable-advised)
A flat round-robin over ~56 DPs at 350 ms = ~20 s per refresh, too slow for battery/status. The
component now has three tiers, scheduled 4 fast slots : 1 slow/static slot:
- **fast** (~3.5 s): DP0, DP1, DP50, DP111, DP209, DP212, DP213, DP221. (DP50 + DP111 are auto-forced
  fast since the cleaning/charging detectors depend on them.)
- **static** (polled at boot, re-polled every `static_interval`, default 10 min): DP54, DP59, DP65,
  DP120, DP205 — confirmed constant (serial/model/config).
- **slow** (~75 s, default): everything else, incl. all the newly-captured unmapped DPs.
A control write bumps DP0/DP1/DP111 to the front of the queue so the UI reacts in well under a second.

## Implementation
`esphome/wybot-dock-bridge.yaml` + `components/wybot_bridge/` implement this: 2 s heartbeat,
round-robin DP polling, report parsing into HA entities, and `write_bool/enum/int` + `send_raw`
API actions. `esphome/wybot-dock-tap.yaml` is a transparent UART0 tap (`uart.debug` + a `tx`
action) for driving the protocol from Python during RE.

## Live cleaning run 2026-11-08 (diagnostic DP mapping, start-control confirmed)
Pressed Start (DP0 write enum 0x03) with robot at 100% docked. Observations over a ~5 min real run:
- **Start control WORKS**: robot launched off the dock and cleaned (battery 100 -> 94% in ~4.5 min,
  i.e. steady motor draw; DP13 bitmap flipped bit31 = `80000000` for a few seconds right after the
  write = command-ACK/starting flag, then cleared).
- **DP0 cleaning_status is unreliable once the robot leaves the dock**: it oscillates Stopped/Cleaning
  (reports `1`=Stopped even while actively cleaning) because the dock relays the robot's DPs over its
  own (acoustic/RF) link and the data goes stale when the robot submerges/moves away. Do NOT trust DP0
  alone for "is it cleaning". `binary_sensor docked`, `DP209`, `DP210/211` all flicker for the same reason.
- **Reliable activity signals**: robot battery (DP50 byte1) monotonically DECREASES under load; **DP111**
  rises fast and monotonically while cleaning (base 21473 -> 28837, +7364 in ~4.5 min ~= +27/s) -> DP111
  is a runtime/pulse counter and the best "actively cleaning" proxy. **DP116** also rises (+581). **DP216**
  rises slowly (+50). 
- **Noisy live-sensor DPs** (oscillate, not counters): DP210 (5..27), DP211 (-1..180), DP217 (259..319),
  DP83 (70..97) -> likely heading / motor-current / orientation telemetry.
- **Constant/config DPs** (unchanged across idle+clean): DP54=50167, DP59=87, DP65=10452, DP120=634,
  DP205=18532800 -> serial/model/config values, not telemetry.
- DP221 solar-dock-battery still `01000A` (byte1=0) through morning; byte2=0x0A. Unresolved.

## HA integration (applied 2026-11-08)
Native vacuum `vacuum.wybot_pool_robot` is a **modern** template vacuum (the legacy `vacuum: platform:
template` format was REMOVED from HA -> errors "must be configured under its own template key instead").
Delivered via a package: `homeassistant: packages: !include_dir_named packages` + `/config/packages/wybot.yaml`
(key `template: - vacuum:`). Modern template vacuum dropped `battery_level` (vacuum battery deprecated;
robot battery is the separate `sensor.wybot_dock_robot_battery`). The `state:` template must emit a bare
VacuumActivity token (use a single trimmed `{{ }}` expression, not a `>` folded if/elif block, or it
renders whitespace -> `unknown`). Modern template entities render on the next source state-change, so the
entity reads `unknown` at boot until a source entity changes (harmless; resolves on first poll delta).
KNOWN REFINEMENT: because DP0 under-reports, the vacuum card can read `idle` mid-clean; a robust "cleaning"
state needs a better signal (battery-falling or DP111-rising latch), TODO.
