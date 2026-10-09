import asyncio
import sys
from bleak import BleakScanner


async def main():
    timeout = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
    devs = await BleakScanner.discover(timeout=timeout, return_adv=True)
    if not devs:
        print("(no advertisements seen)")
    for d, adv in devs.values():
        print(f"{d.address}  rssi={adv.rssi}  name={adv.local_name!r}  svc={list(adv.service_uuids)}")


asyncio.run(main())
