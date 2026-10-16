"""Pair the SK32 BLE keyboard module on Windows using the Python `winrt` projection.

Why Python: the PowerShell 5.1 WinRT projection returns unpopulated DeviceInformationPairing
properties (CanPair/IsPaired read False for *every* device, even genuinely paired ones) and its
scriptblock event handlers never fire on the WinRT thread. The Python `winrt` projection exposes
real property values and invokes event handlers correctly.

Why from_bluetooth_address_async (not DeviceInformation.find_all_async): objects obtained from
FindAllAsync carry no AssociationEndpoint for this device, so their .pairing reports
can_pair=False. The object from from_bluetooth_address_async reports can_pair=True.

Usage:
    python ble_pair.py --addr c2:53:10:8c:eb:3d
"""
import argparse
import asyncio
import sys

from winrt.windows.devices.bluetooth import BluetoothLEDevice
from winrt.windows.devices.enumeration import DevicePairingKinds


def mac_to_int(addr: str) -> int:
    return int(addr.replace(":", ""), 16)


def describe(pairing) -> None:
    print(f"can_pair       : {pairing.can_pair}")
    print(f"is_paired      : {pairing.is_paired}")
    print(f"protection     : {pairing.protection_level}")


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--addr", required=True)
    ap.add_argument("--skip-plain", action="store_true")
    args = ap.parse_args()

    mac = mac_to_int(args.addr)
    dev = await BluetoothLEDevice.from_bluetooth_address_async(mac)
    if dev is None:
        print("DEVICE NOT FOUND (not advertising / out of range)")
        return 2

    di = dev.device_information
    print(f"name           : {di.name}")
    print(f"id             : {di.id}")
    print(f"kind           : {di.kind}")
    describe(di.pairing)

    if di.pairing.is_paired:
        print("ALREADY-PAIRED")
        return 0

    if not args.skip_plain:
        print("\n--- strategy A: plain pair_async (system ceremony) ---")
        res = await di.pairing.pair_async()
        print(f"plain status   : {res.status} ({res.status!r})")
        print(f"is_paired      : {di.pairing.is_paired}")
        if di.pairing.is_paired:
            print("PAIRED-OK(plain)")
            return 0

    print("\n--- strategy B: custom pair_async + PairingRequested handler ---")
    custom = di.pairing.custom
    fired = []

    def on_requested(sender, req):
        fired.append(req.pairing_kind)
        print(f"  [handler] PairingRequested kind={req.pairing_kind}")
        try:
            req.accept()
            print("  [handler] accepted")
        except Exception as e:  # noqa: BLE001
            print(f"  [handler] accept failed: {e!r}")

    token = custom.add_pairing_requested(on_requested)
    kinds = DevicePairingKinds.CONFIRM_ONLY | DevicePairingKinds.CONFIRM_PIN_MATCH
    print(f"kinds          : {kinds}")
    try:
        res = await custom.pair_async(kinds)
        print(f"custom status  : {res.status} ({res.status!r})")
    finally:
        custom.remove_pairing_requested(token)
    print(f"handler fired  : {len(fired)} time(s)")
    print(f"is_paired      : {di.pairing.is_paired}")

    if di.pairing.is_paired:
        print("PAIRED-OK(custom)")
        return 0
    print("PAIRED-FAIL")
    return 2


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
