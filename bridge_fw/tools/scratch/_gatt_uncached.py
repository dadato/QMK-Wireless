"""临时工具：以 UNCACHED 模式发现 BLE GATT 服务，验证 0x1812 是否为缓存陈旧所致。
用法: python _gatt_uncached.py [address]
"""
import asyncio
import sys

from bleak import BleakClient


async def dump(addr, use_cached):
    label = "CACHED" if use_cached else "UNCACHED"
    try:
        async with BleakClient(
            addr, timeout=25.0, winrt={"use_cached_services": use_cached}
        ) as c:
            svcs = getattr(c.services, "services", c.services)
            if isinstance(svcs, dict):
                svcs = list(svcs.values())
            uuids = [str(s.uuid) for s in svcs]
            has1812 = any("1812" in u.lower() for u in uuids)
            print(f"[{label}] connected={c.is_connected} n_svc={len(svcs)} has_0x1812={has1812}")
            for s in svcs:
                print(f"   SVC handle=0x{s.handle:04X}  {s.uuid}")
    except Exception as e:  # noqa: BLE001
        print(f"[{label}] failed: {type(e).__name__} {e}")


async def main():
    addr = sys.argv[1] if len(sys.argv) > 1 else "C1:53:10:8C:EB:3D"
    await dump(addr, False)
    await dump(addr, True)
    await dump(addr, False)


asyncio.run(main())
