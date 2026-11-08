# Comms protocol (datapoint / DP)

The ESP32-C3 gateway relays **Tuya-style datapoint (DP) frames** between the app/cloud and the GD32 motor MCU. The same DP set appears on three transports: MQTT (JSON), BLE (binary), and the internal C3<->MCU UART (binary, inferred). This is what local replacement firmware must speak.

## Transports

### MQTT (JSON over `mqtt://mqtt.wybotpool.com:1883`)
Devices are addressed by id in the topic; the broker uses one shared credential for every device and enforces no per-device ACL (see `ota-security.md`). Payloads carry a `cmd` and a `dp` array.

| Topic (suffix `/{deviceId}`) | Dir | Meaning |
|---|---|---|
| `/device/DATA/recv_transparent_query_data/` | app→dev | query DP(s): `{"ts":..,"cmd":9,"dp":[{"id":N}]}` |
| `/device/DATA/recv_transparent_cmd_data/` | app→dev | write DP(s): `{"ts":..,"cmd":4,"dp":[{"id":N,"type":T,"len":L,"data":"hex"}]}` |
| `/device/DATA/send_transparent_data/` | dev→app | report: `{"cmd":5,"dp":[{id,type,len,data}]}` |
| `/device/OTA/request_all_firmware_info/` | dev→cloud | device lists its boards + versions |
| `/device/OTA/reply_all_firmware_info/` | cloud→app | full firmware entries incl. download url + md5 |
| `/device/OTA/notify_ready_to_update/`, `recv_start_update/`, `post_update_progress/`, `post_errcode/` | OTA | update handshake |
| `/device/lowpower/low_power_cmd/` | dock | solar/low-power control |

`cmd` values: **9** = query, **4** = write, **5** = report.

### BLE
Service/characteristic UUIDs `0000_00ff`/`0000_ff01` and `0000_00ee`/`0000_ee01` (same ones the vendor app uses). Frame format:

```
AA55 | cmd(2 bytes, big-endian) | len(2 bytes, little-endian) | [dp_id, dp_type, dp_len, value...] | checksum(1 byte)
```

### Internal C3 <-> GD32 UART (inferred)
The C3 runs `iot_uart_task` and forwards the `recv_transparent_*` payloads to the motor MCU. The on-wire framing is almost certainly the same `AA55` DP frames as BLE. Replacement firmware re-emits these on the existing UART; exact pins/baud are being extracted from the iot image (and are trivially confirmed with a logic analyzer once the dock is opened).

## DP catalog (from `pywybot` + observed traffic)

| dp_type | meaning |
|---|---|
| 0 | raw / bytes |
| 2 | int |
| 4 | enum |

| dp_id | meaning (observed) |
|---|---|
| 0 | power / start-stop |
| 1 | mode |
| 50, 77 | (seen; semantics TBD) |
| 79 | worktime (4-byte LE) |
| 209, 213 | status enums |
| 222, 225 | (seen; semantics TBD) |

Start/stop, mode select, return-to-dock, and status reads are all expressed as writes/queries over these DP ids. This is sufficient to build local control; remaining dp semantics get filled in by observing live frames.
