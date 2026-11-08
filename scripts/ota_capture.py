#!/usr/bin/env python3
"""
Passively capture a real WyBot OTA `recv_start_update` (and the surrounding OTA handshake)
for ONE device, to confirm the exact message envelope before forging our own.

This only subscribes; it publishes nothing. Run it, then in the official app trigger a
firmware update for your device (or wait for WyBot to push one). It prints every OTA message
for that device id verbatim.

Credentials: shared vendor MQTT credential, provided via env (not shipped). See
`mqtt_catalog_sniff.py` for the variable names.

    export WYBOT_MQTT_HOST=mqtt.wybotpool.com WYBOT_MQTT_USER=... WYBOT_MQTT_PASS=...
    python3 ota_capture.py --device <dockId> --minutes 15
"""
import asyncio, os, sys, time, argparse, json

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", required=True, help="device/dock id (the topic suffix)")
    ap.add_argument("--minutes", type=float, default=15)
    args = ap.parse_args()

    host = os.environ.get("WYBOT_MQTT_HOST", "mqtt.wybotpool.com")
    user, pw = os.environ.get("WYBOT_MQTT_USER"), os.environ.get("WYBOT_MQTT_PASS")
    if not (user and pw):
        sys.exit("Set WYBOT_MQTT_USER / WYBOT_MQTT_PASS (shared vendor credential; not shipped).")
    try:
        import aiomqtt
    except ImportError:
        sys.exit("pip install aiomqtt")

    dev = args.device
    topics = [
        f"/device/OTA/recv_start_update/{dev}",
        f"/device/OTA/notify_ready_to_update/{dev}",
        f"/device/OTA/notify_ready_to_update_reply/{dev}",
        f"/device/OTA/reply_all_firmware_info/{dev}",
        f"/device/OTA/post_update_progress/{dev}",
        f"/device/OTA/post_errcode/{dev}",
    ]
    async with aiomqtt.Client(hostname=host, username=user, password=pw,
                              identifier=f"ota-cap-{int(time.time())}") as c:
        for t in topics:
            await c.subscribe(t)
        print(f"capturing OTA traffic for {dev} ({args.minutes} min). Trigger an update in the app now.", flush=True)

        async def pump():
            async for m in c.messages:
                raw = m.payload.decode(errors="replace")
                try:
                    pretty = json.dumps(json.loads(raw), indent=2)
                except Exception:
                    pretty = raw
                print(f"\n### {m.topic}\n{pretty}", flush=True)

        try:
            await asyncio.wait_for(pump(), timeout=args.minutes * 60)
        except asyncio.TimeoutError:
            pass
    print("capture window ended")

if __name__ == "__main__":
    asyncio.run(main())
