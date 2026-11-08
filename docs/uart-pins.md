# Finding the C3 ↔ GD32 motor-MCU UART pins

The dock's ESP32-C3 talks to the GD32 motor MCU over a UART whose GPIOs are not yet known.
This is the one piece missing from a *fully functional* bridge image (a boot/WiFi/OTA image is
already proven — see `bench-validation.md`). Two no-teardown methods; use them together.

## What static RE established (no dock contact)
From the stock `IOT_DS20` C3 image (disassembled with `riscv32-esp-elf-objdump`, segment bases
from `c3seg/segments.tsv`):

- **The bridge is on UART1.** `0x60010000` (UART1 peripheral base) is referenced; the low-level
  UART1 driver sits in IRAM around `0x403866fc`. (UART0 / USB-CDC is the console.)
- **Signal indices:** `U1RXD_IN_IDX = 9`, `U1TXD_OUT_IDX = 9` (from the IDF `gpio_sig_map.h`).
- **Pin routing is done by inline register writes, not ROM calls.** There are *zero* calls to the
  ROM `gpio_matrix_in` (0x400005a0) / `gpio_matrix_out` (0x400005a4); the only GPIO ROM calls are
  `gpio_output_enable/disable` (0x5ac/0x5a8). So `uart_set_pin` was inlined to direct
  `GPIO_FUNCn_OUT_SEL_CFG` / `GPIO_FUNC9_IN_SEL_CFG` stores. Only three sites load the GPIO-matrix
  base `0x60004xxx`, and none is the UART1 TX/RX routing in a grep-able form — the base is loaded
  once into a callee-saved register and reused with computed offsets inside a pin-config routine.

**Conclusion:** the exact TX/RX GPIO numbers can't be lifted by symbol/opcode grep; it needs a
full decompiler (Ghidra) dataflow pass through that pin-config routine to recover the immediates.
Worth doing only to avoid the scan round-trip below — the scan is faster and also confirms framing.

Candidate GPIOs (what to scan): on a C3-MINI-1, exclude flash (GPIO11–17), USB (18/19), console
UART0 (20/21) and the strapping pins (2/8/9) → **0, 1, 3, 4, 5, 6, 7, 10**.

## The deterministic method: on-dock UART scan (`esphome/wybot-dock-pinscan.yaml`)
After the stage-1 takeover, OTA the dock with the pin-scanner image. The `wybot_pinscan` component
walks each candidate GPIO × baud on UART1, listening **passively** (TX is never driven — no bus
contention), and logs which pin/baud carries Tuya `AA55` DP frames — that pin is the **C3 RX**
(= the MCU's TX). It hex-dumps what it hears, so the real framing, baud and DP ids are captured at
the same time.

**The scan is disarmed at boot and self-gating.** On power-up the image is pure connectivity
(WiFi + API + OTA) and touches no GPIO, so a sealed dock first proves it is reachable. Probing
starts only once you turn on the **Pin scan enabled** switch AND a HA/API client is connected, and
it stops the instant either goes away — a reset can never start a scan unattended (the switch is
`restore_mode: ALWAYS_OFF`). The candidate list is an allow-list in `__init__.py` that refuses the
flash pins (11–17) and USB-JTAG pins (18–19). At boot it also logs the partition table and the
running/next-OTA slot, and exposes a `boot_other_slot` API action that flips back to the stock slot
with zero upload.

```
esphome run esphome/wybot-dock-pinscan.yaml --device <dock-ip>   # watch for '** HIT **'
# then, in HA: turn on "Pin scan enabled" (stay connected) to arm the passive scan
```

Then:
1. The HIT line gives **RX GPIO + baud**.
2. Find **TX**: it is almost always the header pin paired with RX. Confirm by setting it in
   `wybot-dock.yaml`'s `uart:` block and having `wybot_bridge` send a DP query — a reply frame on
   RX confirms the pair.
3. Set the discovered `tx_pin` / `rx_pin` / `baud_rate` in `wybot-dock.yaml`, enable `wybot_bridge`,
   and OTA the real functional image.

The hardened scanner image **compiles** (bench-verified, ~900 KB, 49% flash) and is ready to flash.
