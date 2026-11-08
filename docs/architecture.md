# Architecture: where the WiFi brain lives

**Question:** to get local control, do we reflash the *robot* or the *dock*?
**Answer: the dock.** Its ESP32-C3 is the single WiFi/cloud brain; the robot is a bare-metal GD32 motor MCU with no network stack.

## Evidence

### 1. The `iot` image is the DS20 dock's image, and it is a full ESP32-C3 app
`esptool image-info` on `IOT_DS20_C3MINI_H0_2025_06_09_V2.2.20.bin`:

- Chip ID 5 = **ESP32-C3**, project name **`IOT_main`**, ESP-IDF **v5.1.2**, compiled Jun 9 2025.
- Segments map to C3 memory (DROM `0x3c11_0020`, IRAM `0x4038_0000`, IROM `0x4200_0020`), entry `0x4038_040e`.
- **`Secure version: 0`**, checksum + validation hash valid. No signed-image footer.

### 2. That one image contains the entire cloud path *and* the motor bridge
Strings from the iot image:

- **WiFi/network stack:** `handle_wifi_sta.c`, `esp_wifi/src/wifi_init.c`, `wpa_supplicant`, full `lwip` (DHCP client *and* a DHCP **server** + `ip4_napt.c`, i.e. the provisioning soft-AP/router).
- **Cloud clients:** `mqtt://mqtt.wybotpool.com`, `https://api.wybotpool.com/api/device/login`, `https://api.wybotpool.com/api/docker/` ("docker" = the dock), `time.apple.com` SNTP.
- **Topics:** `/device/DATA/recv_transparent_cmd_data/`, `/device/DATA/recv_transparent_query_data/`, `/device/DATA/send_transparent_data/`, `/device/OTA/*`, `/device/lowpower/low_power_cmd/`.
- **Motor-MCU UART bridge:** task string **`iot_uart_task`**, plus the `recv_transparent_*` / `send_transparent_data` relay. The C3 takes DP commands from WiFi/BLE and forwards them over UART to the GD32.

A single board that is simultaneously the WiFi station, the MQTT/REST cloud client, the BLE endpoint, and the UART master to the motor MCU **is** the comms gateway. It is the DS20 image, so it is in the dock.

### 3. The robot and the two other dock boards have no network stack
`WYBOT_S2P_main` (robot), `DS20_Lower_MCU` (sonar), `DS20_Upper_MCU` (upper) are GD32 ARM Cortex-M3 images:

- Each begins with a **4-byte little-endian image-length prefix**, then a Cortex-M vector table loaded at `0x0800_0000`:
  - robot: SP `0x2001_8000`, reset `0x0801_f77d`
  - sonar/lower: SP `0x2000_8000`, reset `0x0800_47d1`
  - upper: SP `0x2000_2000`, reset `0x0800_4a0d`
- **Zero** WiFi / ESP / lwip / MQTT strings in any of them. Bare-metal GD32 firmware.

So the robot cannot reach WiFi on its own; it depends on the dock's C3. (When the robot is cleaning in the pool it talks to the dock over BLE/RF, which is part of why in-pool control feels laggy; scheduled cleaning and status flow through the dock gateway + the vendor cloud, which is the round-trip we remove.)

## Consequence for the plan
Reflash **only the dock's ESP32-C3** with local firmware that:
1. joins the home WiFi (or exposes a local API / BLE),
2. re-emits the same DP frames on the existing C3<->GD32 UART,
so the motor MCU and all cleaning behavior are untouched. Everything else (robot MCU, sonar MCU, charging MCU) stays stock.

## Chip memory map used for board role-ID (GD32E103 / STM32F103-clone)
APB1 `0x4000_0000` (TIM2-7, USART2-5, I2C, SPI2/3, CAN, PWR, DAC) · APB2 `0x4001_0000` (AFIO/EXTI, GPIOA-E `0x4001_0800`+, ADC1/2 `0x4001_2400`, TIM1 `0x4001_2C00`, SPI1, USART1 `0x4001_3800`) · AHB `0x4002_0000` (DMA, RCC `0x4002_1000`, FMC, CRC). Peripheral usage per board is being read from the decompiled images to confirm roles (motor PWM timers, ADC current/battery sense, sonar input-capture timers).
