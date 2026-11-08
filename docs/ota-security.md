# Firmware delivery & integrity

## How firmware is delivered
- The vendor app triggers a firmware check; the device publishes `request_all_firmware_info` and the cloud answers `reply_all_firmware_info` with, per board: `boardID, productModel, chipType, softwareVer, hardwareVer, url, md5, release notes, dependency`.
- `url` points at **`http://gateway.wybotpool.com/...`** — plain HTTP, **no authentication**, predictable filenames. Anyone can download any model's firmware. `firmware/manifest.tsv` is the harvested index.
- OTA also has a **BLE push path** (app streams the image to the device in chunks), which is a potential no-open reflash channel for custom firmware (pending device-side verification analysis).

## Integrity (what stops a custom image?)
- Images are **plaintext**. The ESP32-C3 image is a standard ESP-IDF app (magic `0xE9`, app-descriptor `0xABCD5432`), `Secure version: 0`, no signed footer.
- Integrity at the app/OTA layer is **MD5 + CRC32 only** — no RSA/ECDSA signature. The vendor app logs `server md5:` / `local md5:`, `MD5 verification failed`, `CRC Ok` / `CRC Error`. The AES/RSA present in the app is generic library code (local DB / TLS), not firmware signing.
- Therefore a self-built image passes the vendor's own integrity checks. The **one remaining unknown** is whether the ESP32-C3 bootloader independently enforces **Secure Boot v2 / flash encryption** via eFuses. That is settled only by reading the eFuse summary (`esptool get_security_info`) once the dock is opened and a UART is attached. `Secure version: 0` and the plaintext image strongly suggest it does not, but this is not yet proven.

## Responsible disclosure note (vendor-side issues, documented without the live secret)
- The MQTT backend (`mqtt.wybotpool.com:1883`) uses a **single shared username/password hardcoded into every device/app build**, and devices are addressed solely by id in the topic path with **no per-device authorization**. Any party with the shared credential can subscribe to, and publish to, any device's topics by id. The live credential is intentionally **omitted** from this repo.
- Firmware is served **unauthenticated over plain HTTP** (no TLS, no signature), so images can be enumerated, downloaded, and are susceptible to tampering in transit.

These should be reported to WyBot. This repo exists for interoperability/repair of author-owned hardware and does not include anything required to target other users' devices.
