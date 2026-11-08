#!/usr/bin/env python3
"""
Passively capture a dock's own firmware report (installed board versions, productModel,
chipType) and the cloud's firmware-offer envelope. Subscribe-only; publishes NOTHING.

The dock PUBLISHES its installed versions on   /device/OTA/request_all_firmware_info/<id>
The cloud REPLIES with the offer (url+md5) on   /device/OTA/reply_request_all_firmware_info/<id>
The cloud may also push                          /device/OTA/reply_all_firmware_info/<id>

Run this, then trigger a firmware check for the dock in the official WyBot app (or power-cycle
the dock so it re-checks on boot). We publish nothing; the app/cloud does the triggering.

    export WYBOT_MQTT_HOST=mqtt.wybotpool.com WYBOT_MQTT_PORT=1883
    export WYBOT_MQTT_USER=... WYBOT_MQTT_PASS=...     # shared vendor credential, not shipped
    python3 fw_info_capture.py --device <id> --minutes 10
    python3 fw_info_capture.py --discover --minutes 10   # wildcard: learn the id if --device is wrong

In --discover mode we subscribe to the firmware-info wildcards and print, for every device,
only its id + per-board {boardID, hardwareVer, softwareVer, productModel, chipType} plus any
url/md5 (vendor firmware-catalog data). Trigger only your own dock so the correlated burst is
yours; the topic suffix reveals the true id.
"""
import asyncio, os, sys, time, argparse, json

INFO_TOPICS = [
    "/device/OTA/request_all_firmware_info/",
    "/device/OTA/reply_request_all_firmware_info/",
    "/device/OTA/reply_all_firmware_info/",
]
EXTRA_TOPICS = [
    "/device/OTA/recv_start_update/",
    "/device/OTA/notify_ready_to_update/",
    "/device/OTA/notify_ready_to_update_reply/",
    "/device/OTA/post_update_progress/",
    "/device/OTA/post_errcode/",
]

def summarize(topic, raw, logf):
    try:
        d = json.loads(raw)
    except Exception:
        line = f"\n### {topic}\n{raw}"
        print(line, flush=True); logf.write(line + "\n"); logf.flush(); return
    fw = d.get("firmware")
    if isinstance(fw, list):
        rows = []
        for e in fw:
            rows.append({k: e.get(k) for k in
                         ("boardID", "productModel", "chipType", "hardwareVer",
                          "softwareVer", "url", "md5", "MD5", "manual")
                         if e.get(k) is not None})
        body = json.dumps(rows, indent=2)
    else:
        body = json.dumps(d, indent=2)
    ts = time.strftime('%H:%M:%S')
    line = f"\n### [{ts}] {topic}\n{body}"
    print(line, flush=True); logf.write(line + "\n"); logf.flush()

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default=None, help="exact dock id (topic suffix)")
    ap.add_argument("--discover", action="store_true", help="wildcard mode to learn the id")
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--prefix", default=None, help="in discover mode, only print ids whose topic suffix starts with this")
    ap.add_argument("--payload-grep", default=None, help="only print messages whose raw payload contains this substring")
    ap.add_argument("--out", default="fw_info_capture.log")
    args = ap.parse_args()
    if not args.device and not args.discover:
        sys.exit("give --device <id> or --discover")

    host = os.environ.get("WYBOT_MQTT_HOST", "mqtt.wybotpool.com")
    port = int(os.environ.get("WYBOT_MQTT_PORT", "1883"))
    user, pw = os.environ.get("WYBOT_MQTT_USER"), os.environ.get("WYBOT_MQTT_PASS")
    if not (user and pw):
        sys.exit("Set WYBOT_MQTT_USER / WYBOT_MQTT_PASS (shared vendor credential; not shipped).")
    import aiomqtt

    if args.discover:
        subs = [t + "#" for t in INFO_TOPICS]
    else:
        subs = [t + args.device for t in INFO_TOPICS + EXTRA_TOPICS]

    logf = open(args.out, "a")
    async with aiomqtt.Client(hostname=host, port=port, username=user, password=pw,
                              identifier=f"fwinfo-{int(time.time())}-{os.getpid()}") as c:
        for t in subs:
            await c.subscribe(t)
        banner = (f"[{time.strftime('%H:%M:%S')}] subscribed ({'discover' if args.discover else args.device}); "
                  f"{args.minutes} min. Trigger a firmware check for the dock in the app now.")
        print(banner, flush=True); logf.write(banner + "\n"); logf.flush()

        async def pump():
            async for m in c.messages:
                t = str(m.topic)
                if args.prefix:
                    suffix = t.rsplit("/", 1)[-1]
                    if not suffix.startswith(args.prefix):
                        continue
                raw = m.payload.decode(errors="replace")
                if args.payload_grep and args.payload_grep not in raw:
                    continue
                summarize(t, raw, logf)

        try:
            await asyncio.wait_for(pump(), timeout=args.minutes * 60)
        except asyncio.TimeoutError:
            pass
    logf.write(f"[{time.strftime('%H:%M:%S')}] window ended\n"); logf.close()
    print("capture window ended", flush=True)

if __name__ == "__main__":
    asyncio.run(main())
