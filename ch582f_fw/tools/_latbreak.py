"""临时：拆分上行时延各段（WCH-Link 串口 RTT / Dongle HID RTT）。用完即删。"""
from __future__ import annotations
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serial
from sk32sim import proto as P

KEYBD = "COM13"
BAUD = 921600


def stats(name, xs):
    xs = sorted(xs)
    n = len(xs)
    print(f"  {name:22s} n={n:4d} 平均 {sum(xs)/n:6.3f} 中位 {xs[n//2]:6.3f} "
          f"P5 {xs[int(n*0.05)]:6.3f} P95 {xs[int(n*0.95)]:6.3f} 最小 {xs[0]:6.3f} 最大 {xs[-1]:6.3f} ms")


def module_serial_rtt(n=200):
    """COM10：写 CMD_GET_STATUS（模组本地应答，不经射频）→ 读回。"""
    ser = serial.Serial(KEYBD, BAUD, bytesize=8, parity="N", stopbits=1,
                        timeout=0, write_timeout=2.0)
    parser = P.FrameParser()
    out = []
    wout = []
    try:
        ser.reset_input_buffer()
        for i in range(n):
            seq = i & 0xFF
            tw = time.monotonic()
            ser.write(P.encode(P.CMD_GET_STATUS, seq, 0, b""))
            wout.append((time.monotonic() - tw) * 1000.0)
            t0 = time.monotonic()
            got = False
            while time.monotonic() - t0 < 0.05:
                nw = ser.in_waiting
                if nw:
                    for fr in parser.feed(ser.read(nw)):
                        if fr.cmd == P.CMD_GET_STATUS:
                            got = True
                if got:
                    break
                time.sleep(0.0002)
            if got:
                out.append((time.monotonic() - t0) * 1000.0)
    finally:
        ser.close()
    stats("模组串口 write()耗时", wout)
    stats("模组串口 write后RTT", out)
    stats("模组串口 全RTT(write+回)", [a + b for a, b in zip(wout, out)])
    return out


def dongle_hid_rtt(n=200):
    """Dongle HID：写 OTA GET_INFO（Dongle 本地应答）→ 读 EP3 IN 回包。"""
    import hid
    path = None
    for d in hid.enumerate():
        p = d["path"].decode(errors="replace")
        if d.get("vendor_id") == 0x0194 and d.get("product_id") == 0x0463 \
                and d.get("usage_page") == 0xFF60 and "mi_02" in p.lower():
            path = d["path"]
            break
    if path is None:
        print("  Dongle HID 未找到")
        return []
    h = hid.device()
    h.open_path(path)
    out = []
    wout = []
    try:
        while h.read(64, 20):
            pass
        for _ in range(n):
            tw = time.monotonic()
            h.write(b"\x00" + b"OTA" + bytes([0x06]) + bytes(28))
            wout.append((time.monotonic() - tw) * 1000.0)
            t0 = time.monotonic()
            for _ in range(50):
                data = h.read(64, 5)
                if data:
                    out.append((time.monotonic() - t0) * 1000.0)
                    break
            while h.read(64, 1):   # 清残留
                pass
    finally:
        h.close()
    stats("Dongle HID write()耗时", wout)
    stats("Dongle HID write后RTT", out)
    stats("Dongle HID 全RTT(write+回)", [a + b for a, b in zip(wout, out)])
    return out


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    module_serial_rtt(n)
    dongle_hid_rtt(n)
