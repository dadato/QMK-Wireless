"""Windows BLE unpair helper (WinRT DeviceInformationPairing.UnpairAsync).

Why Python: same reason as ble_pair.py -- the PowerShell 5.1 WinRT projection does not
populate pairing properties / fire handlers. from_bluetooth_address_async yields an object
that carries a real AssociationEndpoint, so its .pairing.unpair_async() actually works.

Usage:
    python _ble_unpair.py --addr c1:53:10:8c:eb:3d
"""
import argparse
import asyncio
import sys

from winrt.windows.devices.bluetooth import BluetoothLEDevice


def mac_to_int(addr: str) -> int:
    return int(addr.replace(":", ""), 16)


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", required=True)
    args = ap.parse_args()

    dev = await BluetoothLEDevice.from_bluetooth_address_async(mac_to_int(args.addr))
    if dev is None:
        print("DEVICE NOT FOUND (not advertising / out of range)")
        return 2

    di = dev.device_information
    print(f"name           : {di.name}")
    print(f"id             : {di.id}")
    print(f"is_paired      : {di.pairing.is_paired}")
    print(f"can_pair       : {di.pairing.can_pair}")

    if not di.pairing.is_paired:
        print("NOT-PAIRED (nothing to do)")
        return 0

    res = await di.pairing.unpair_async()
    print(f"unpair status  : {res.status} ({res.status!r})")
    print(f"is_paired      : {di.pairing.is_paired}")
    if not di.pairing.is_paired:
        print("UNPAIRED-OK")
        return 0
    print("UNPAIR-FAIL")
    return 2


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
