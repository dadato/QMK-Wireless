"""临时工具：按句柄直接读 ATT 属性，判定服务是否真的在服务端数据库里。

服务发现(Read By Group Type)若漏报某服务，但该服务其实已注册，则直接按句柄
ATT_READ 仍能读回其服务声明 UUID（0x2800 的值 = 服务 UUID）。若读回错误，
说明该句柄在服务端属性库里不存在（服务未真正入表）。

用法: python _att_probe.py [address] [lo] [hi]
"""
import asyncio
import sys

from bleak import BleakClient


async def main():
    addr = sys.argv[1] if len(sys.argv) > 1 else "C1:53:10:8C:EB:3D"
    lo = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x20
    hi = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x60
    async with BleakClient(addr, timeout=25.0) as c:
        print("connected:", c.is_connected)
        for h in range(lo, hi + 1):
            try:
                v = await c.read_gatt_char(h)
                print(f"0x{h:04X}  OK   {v.hex(' ')}")
            except Exception as e:  # noqa: BLE001
                print(f"0x{h:04X}  ERR  {type(e).__name__}: {e}")


asyncio.run(main())
