#!/usr/bin/env python3
"""
Passively harvest WyBot's firmware catalog from its MQTT backend.

The vendor cloud answers a device's firmware check on `/device/OTA/reply_all_firmware_info/#`
with full entries including the plain-HTTP download url + md5 for every board. Subscribing to
that wildcard (while you trigger a firmware check in the official app on your OWN device) yields
the download URLs. This is read-only; it does not publish anything.

Credentials: the vendor uses a single shared MQTT credential baked into every build. It is NOT
included here. Provide it via environment:

    export WYBOT_MQTT_HOST=mqtt.wybotpool.com
    export WYBOT_MQTT_USER=...      # shared vendor username
    export WYBOT_MQTT_PASS=...      # shared vendor password
    python3 mqtt_catalog_sniff.py --minutes 10 --out catalog.tsv

Requires: pip install aiomqtt
"""
import asyncio, json, os, time, argparse, sys

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--out", default="catalog.tsv")
    args = ap.parse_args()

    host = os.environ.get("WYBOT_MQTT_HOST", "mqtt.wybotpool.com")
    user = os.environ.get("WYBOT_MQTT_USER")
    pw   = os.environ.get("WYBOT_MQTT_PASS")
    if not (user and pw):
        sys.exit("Set WYBOT_MQTT_USER and WYBOT_MQTT_PASS (shared vendor credential; not shipped).")

    try:
        import aiomqtt
    except ImportError:
        sys.exit("pip install aiomqtt")

    seen = set()
    with open(args.out, "a") as f:
        if f.tell() == 0:
            f.write("boardID\tproductModel\tchipType\thardwareVer\tsoftwareVer\turl\tmd5\n")

    async with aiomqtt.Client(hostname=host, username=user, password=pw,
                              identifier=f"catalog-{int(time.time())}") as c:
        await c.subscribe("/device/OTA/reply_all_firmware_info/#")
        await c.subscribe("/device/OTA/request_all_firmware_info/#")
        print(f"listening {args.minutes} min — now trigger a firmware check in the app for your device", flush=True)

        async def pump():
            async for m in c.messages:
                try:
                    d = json.loads(m.payload.decode())
                except Exception:
                    continue
                for e in d.get("firmware", []):
                    url = (e.get("url") or "").strip()
                    if not url.startswith("http") or url in seen:
                        continue
                    seen.add(url)
                    row = [e.get("boardID",""), e.get("productModel",""), e.get("chipType",""),
                           e.get("hardwareVer",""), e.get("softwareVer",""), url, e.get("md5","")]
                    with open(args.out, "a") as f:
                        f.write("\t".join(str(x) for x in row) + "\n")
                    print("NEW", row[0], row[1], row[4], url, flush=True)

        try:
            await asyncio.wait_for(pump(), timeout=args.minutes * 60)
        except asyncio.TimeoutError:
            pass
    print(f"done — {len(seen)} unique firmware URLs -> {args.out}")

if __name__ == "__main__":
    asyncio.run(main())
