"""临时工具：连接 BLE 外设并打印 GATT 服务/特征表（含句柄，用于核对 ATT 库）。
用法: python _gatt_dump.py [address]
"""
import asyncio
import sys

from bleak import BleakClient


async def main():
    addr = sys.argv[1] if len(sys.argv) > 1 else "C1:53:10:8C:EB:3D"
    try:
        async with BleakClient(addr, timeout=25.0) as c:
            print("connected:", c.is_connected)
            svcs = getattr(c.services, "services", c.services)
            if isinstance(svcs, dict):
                svcs = list(svcs.values())
            prev_last = 0
            for s in svcs:
                print(f"SVC  handle=0x{s.handle:04X}  {s.uuid}  ({s.description})")
                if prev_last:
                    gap = s.handle - prev_last - 1
                    if gap:
                        print(f"     ^ 上一服务末句柄后空缺 {gap} 个句柄")
                chs = list(s.characteristics)
                for ch in chs:
                    print(f"   CH  handle=0x{ch.handle:04X}  {ch.uuid}  props={ch.properties}")
                    for d in ch.descriptors:
                        print(f"      DESC handle=0x{d.handle:04X}  {d.uuid}")
                last = s.handle
                for ch in chs:
                    last = max(last, ch.handle)
                    for d in ch.descriptors:
                        last = max(last, d.handle)
                prev_last = last
            print("total_services:", len(svcs))
    except Exception as e:  # noqa: BLE001
        print("dump failed:", type(e).__name__, e)


asyncio.run(main())
