"""桥链路探针 —— 判定目标板 UART1 是否存活。

用法:
    python probe.py COM10
    python probe.py COM10 --listen 5 --burst 2

行为:
  1) 纯被动监听 N 秒（不写），打印收到的原始字节与解析到的帧；
  2) 对一组「探针命令」逐条做 1ms 间隔密发 burst 秒，打印是否有任何回包。
"""
from __future__ import annotations

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from sk32sim import proto  # noqa: E402


PROBES = [
    (proto.CMD_PING, b""),
    (proto.CMD_GET_VERSION, b""),
    (proto.CMD_GET_STATUS, b""),
    (proto.CMD_GET_MODE, b""),
    (proto.CMD_GET_WIRELESS_STATE, b""),
    (proto.CMD_SLEEP, bytes((proto.SLEEP_WAKE,))),
    (proto.CMD_ENTER_BOOT, b""),
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--listen", type=float, default=3.0)
    ap.add_argument("--burst", type=float, default=1.5)
    args = ap.parse_args()

    import serial

    try:
        ser = serial.Serial(args.port, args.baud, bytesize=8, parity="N",
                            stopbits=1, timeout=0, write_timeout=2.0)
    except Exception as e:  # noqa: BLE001
        print(f"OPEN FAIL {args.port}: {e}")
        return 2

    parser = proto.FrameParser()
    total_rx = 0

    def drain(seconds: float, tag: str) -> int:
        nonlocal total_rx
        t0 = time.monotonic()
        got = 0
        while time.monotonic() - t0 < seconds:
            n = ser.in_waiting
            if n:
                data = ser.read(n)
                got += len(data)
                total_rx += len(data)
                frames = parser.feed(data)
                print(f"  [{tag}] RAW {data.hex(' ')}")
                for fr in frames:
                    print(f"  [{tag}] FRAME {fr!r}")
            else:
                time.sleep(0.005)
        return got

    print(f"== {args.port} @ {args.baud} ==")
    print(f"[1] 被动监听 {args.listen}s ...")
    got = drain(args.listen, "listen")
    print(f"    被动监听收到 {got} 字节")

    seq = 0
    for cmd, payload in PROBES:
        seq = (seq + 1) & 0xFF
        raw = proto.encode(cmd, seq, 0, payload)
        n = 0
        t0 = time.monotonic()
        while time.monotonic() - t0 < args.burst:
            ser.write(raw)
            n += 1
            time.sleep(0.001)
        got = drain(0.25, proto.cmd_name(cmd))
        print(f"[2] {proto.cmd_name(cmd):18s} 密发 {n:5d} 帧 -> 回包 {got} 字节")

    print(f"== 合计收到 {total_rx} 字节, CRC 错 {parser.crc_errors} ==")
    ser.close()
    return 0 if total_rx else 1


if __name__ == "__main__":
    raise SystemExit(main())
