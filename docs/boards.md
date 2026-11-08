# Per-board decompile results

All four images were decompiled (Ghidra headless; the GD32 ARM images as `ARM:LE:32:Cortex`
loaded at `0x0800_0000`, the ESP32-C3 image as `RISCV:LE:32` with each flash segment mapped to
its load address). Summary of what each board is and does. The decompiled C itself is not
committed (it is a derivative of WyBot's firmware); this is the findings digest.

## iot — ESP32-C3-MINI-1 (DS20 dock)  **[REPLACE target]**
- `IOT_DS20_C3MINI_H0_2025_06_09_V2.2.20.bin`, ESP-IDF v5.1.2 app, project `IOT_main`, ~31.8k lines of decompiled C.
- Role: WiFi station + provisioning soft-AP, MQTT/REST cloud client, BLE endpoint, and UART master bridging to the GD32 motor MCU. See `architecture.md`.
- Integrity: plaintext, `Secure version: 0`, no signed footer (see `ota-security.md`).
- UART-to-MCU: the image contains `iot_uart_task` but **no `driver/uart/uart.c` strings**, i.e. the bridge drives the UART via the HAL/registers directly rather than the stock IDF driver. Exact TX/RX GPIO + baud are therefore not a simple driver-call read; they come from an interactive xref of `iot_uart_task` (future deep-RE pass) or, more cheaply, a logic-analyzer capture once the dock is opened.

## robot — GD32E103 (`WYS2P` = S2 Pro)  **[keep]**
- `WYBOT_S2P_main_V1.7.22_new.bin`, Cortex-M3, SP `0x2001_8000` (≈96 KB SRAM → largest/most complex of the MCUs), reset `0x0801_f77d`, 209 functions.
- Confirmed direct peripheral use: **RCC** (clock tree), **TIM2**, **GPIOA** (plus more reached via HAL base+offset that Ghidra leaves unresolved without an SVD).
- Role: robot motor / pump / navigation controller. This is the board whose behavior we deliberately leave untouched.

## sonar_station — GD32E103 (DS20 dock lower MCU)  **[keep]**
- `DS20_Lower_MCU_V1.5.6_new.bin`, Cortex-M3, SP `0x2000_8000`, reset `0x0800_47d1`, 71 functions.
- Confirmed direct peripheral use: **RCC**, **TIM2**.
- Role (from catalog `boardID=sonar_station` + size): dock sonar / positioning (ultrasonic ranging to locate the robot at the dock).

## "9" — GD32E103 (DS20 dock upper MCU)  **[keep]**
- `DS20_Upper_MCU_V1.6.6_new.bin`, Cortex-M3, SP `0x2000_2000` (smallest), reset `0x0800_4a0d`, 66 functions.
- Role (from catalog `boardID=9` + the dock's `/device/lowpower/low_power_cmd/` MQTT topic): dock charging / solar power management.

## Method notes
- GD32 images carry a 4-byte little-endian length prefix before the Cortex-M vector table; strip it so file offset 0 maps to `0x0800_0000`.
- Full peripheral role-ID on the GD32 boards would need an STM32F103/GD32E103 SVD applied in Ghidra (HAL code passes peripheral struct pointers, so most MMIO is base+offset, not absolute). Not pursued: these are keep-as-is boards and naming + confirmed peripherals already fix their roles.
- Apple-Silicon note: Ghidra 12.1.x ships no macOS-arm64 native decompiler; build it from `Ghidra/Features/Decompiler/src/decompile/cpp` with `make ghidra_opt ARCH_TYPE="-arch arm64"` and install to `os/mac_arm_64/decompile`.
