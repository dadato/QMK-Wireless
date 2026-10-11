"""临时工具：经 USB vendor HID(EP3, RID0, 32B) 刷 Dongle App（App 侧 OTA 方案A）。

协议（port/ch58x/bridge_usb_ch58x.c 的 usb_ota_handle_sub）：
  写 33B = [0x00 RID] + 'O','T','A' + [SUB] + ARGS(<=28)，设备收到 32B 数据；
  IN 回 [status u8][data...]，status 0 = OK。
  SUB: 0x01 ENTER / 0x02 FW_BEGIN(total4+crc2) / 0x03 FW_DATA(off4+data<=24)
       / 0x04 FW_END / 0x05 FW_ABORT / 0x06 GET_INFO
App 直接收包写 Stage；FW_END 校验通过 → 写 IAP 标志(0x03) 复位 → 常驻 BOOT 把
Stage(0x38000) 搬到 APP_A(0x2000) → 跳新 App。

用法: python _dfw.py [bin路径]  （默认 build/dongle-release/bridge_dongle.bin）
"""
from __future__ import annotations

import os
import sys
import time

import hid  # hidapi

CHUNK = 24                      # App 侧 FW_DATA 单帧有效数据上限（BRIDGE_OTA_HID_CHUNK）
MAX_IMAGE = 216 * 1024
DATA_RETRY = 8
ACK_TMO_MS = 3000

VID, PID, USAGE_PAGE = 0x0194, 0x0463, 0xFF60


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE（init 0xFFFF / poly 0x1021 / 不反转 / xorout 0），
    与 core/bridge_crc16.c、board/ota_web/js/crc16.js 逐位一致。"""
    c = 0xFFFF
    for b in data:
        c ^= (b << 8) & 0xFFFF
        for _ in range(8):
            c = (((c << 1) ^ 0x1021) & 0xFFFF) if (c & 0x8000) else ((c << 1) & 0xFFFF)
    return c


def open_dev():
    for d in hid.enumerate():
        p = d["path"].decode(errors="replace")
        if (d.get("vendor_id") == VID and d.get("product_id") == PID
                and d.get("usage_page") == USAGE_PAGE and "mi_02" in p.lower()):
            h = hid.device()
            h.open_path(d["path"])
            print(f"[hid] {p}")
            return h
    return None


def drain(h, ms: int = 10) -> None:
    while h.read(64, ms):
        pass


def _body(raw: bytes) -> bytes:
    """归一化 IN 应答：去掉可能的 RID 前缀，返回 32B body（body[0]=status）。"""
    if len(raw) >= 33 and raw[0] == 0x00:
        return raw[1:33]
    if len(raw) >= 32:
        return raw[:32]
    return raw


def request(h, sub: int, args: bytes = b"", tmo_ms: int = ACK_TMO_MS, retries: int = 1):
    args = bytes(args)[:28]
    req = (b"\x00" + b"OTA" + bytes([sub]) + args)
    req = req + bytes(33 - len(req))
    last = None
    for attempt in range(retries + 1):
        drain(h)
        h.write(req)
        t0 = time.monotonic()
        while (time.monotonic() - t0) * 1000 < tmo_ms:
            data = h.read(64, tmo_ms)
            if data:
                b = _body(bytes(data))
                return b
        last = None
        if attempt < retries:
            time.sleep(0.05 + 0.05 * attempt)
    return last


def get_info(h):
    b = request(h, 0x06, b"", 1500, 2)
    if not b:
        return None
    return {
        "status": b[0],
        "mode": b[1] if len(b) > 1 else -1,
        "fw": (b[2] | (b[3] << 8)) if len(b) > 3 else -1,
        "vid": (b[4] | (b[5] << 8)) if len(b) > 5 else -1,
        "pid": (b[6] | (b[7] << 8)) if len(b) > 7 else -1,
    }


def main() -> int:
    assert crc16(b"123456789") == 0x29B1, "CRC 自检失败"

    path = sys.argv[1] if len(sys.argv) > 1 else "build/dongle-release/bridge_dongle.bin"
    if not os.path.isfile(path):
        print(f"找不到镜像: {path}")
        return 2
    raw = open(path, "rb").read()
    if len(raw) == 0 or len(raw) > MAX_IMAGE:
        print(f"镜像大小非法: {len(raw)}")
        return 2

    pad = (CHUNK - (len(raw) % CHUNK)) % CHUNK
    data = raw + b"\xFF" * pad
    total = len(data)
    crc = crc16(data)
    print(f"[img] {path}\n      原始 {len(raw)}B -> 补齐 {total}B (pad {pad})  CRC=0x{crc:04X}")

    h = open_dev()
    if h is None:
        print("未找到 Dongle vendor HID (MI_02 / Usage 0xFF60)")
        return 3
    try:
        info = get_info(h)
        print(f"[dev] GET_INFO: {info}")
        if info is None or info["status"] != 0:
            print("GET_INFO 无应答/异常，放弃")
            return 4
        if info["mode"] != 0:
            print(f"注意: 设备 mode={info['mode']}（非 App=0），仍尝试。")

        # ---- FW_BEGIN ----
        args = total.to_bytes(4, "little") + crc.to_bytes(2, "little")
        b = request(h, 0x02, args, 2500, 2)
        if not b or b[0] != 0:
            print(f"FW_BEGIN 失败: {b[:4].hex(' ') if b else '无应答'}")
            return 5
        chunk_max = b[1] if len(b) > 1 else 0
        n = min(chunk_max, CHUNK) if chunk_max else CHUNK
        n -= n % 4
        n = n or 4
        print(f"[ota] FW_BEGIN ok, chunk_max={chunk_max} 分片={n}B/帧")

        # ---- FW_DATA ----
        t0 = time.monotonic()
        frames = (total + n - 1) // n
        for idx, off in enumerate(range(0, total, n)):
            blk = data[off:off + n]
            a = off.to_bytes(4, "little") + blk
            ok = False
            for attempt in range(DATA_RETRY + 1):
                rb = request(h, 0x03, a, ACK_TMO_MS, 0)
                if rb and rb[0] == 0:
                    ok = True
                    break
                time.sleep(0.02 + 0.02 * attempt)
            if not ok:
                print(f"\nFW_DATA off={off} 失败(重试 {DATA_RETRY} 次): "
                      f"{rb[:4].hex(' ') if rb else '无应答'}")
                return 6
            if idx % 500 == 0 or idx == frames - 1:
                pct = (off + len(blk)) / total * 100.0
                print(f"  {pct:5.1f}%  off={off + len(blk)}/{total}  "
                      f"{time.monotonic() - t0:.1f}s", flush=True)

        # ---- FW_END ----
        rb = request(h, 0x04, b"", 4000, 0)
        print(f"[ota] FW_END resp: {rb[:4].hex(' ') if rb else '(无应答，设备可能已复位)'}")
        if rb and rb[0] != 0:
            print(f"FW_END 返回错误 status={rb[0]:#04x}")
            return 7
    finally:
        try:
            h.close()
        except Exception:
            pass

    # ---- 等复位重枚举后复核 ----
    print("[ota] 等待搬运并复位（8s）...")
    time.sleep(8.0)
    for _ in range(20):
        h2 = open_dev()
        if h2 is not None:
            try:
                info2 = get_info(h2)
                if info2 and info2["status"] == 0:
                    print(f"[post] GET_INFO: {info2}")
                    return 0
            finally:
                try:
                    h2.close()
                except Exception:
                    pass
        time.sleep(1.0)
    print("[post] 未能复核（设备未重枚举？）")
    return 8


if __name__ == "__main__":
    sys.exit(main())
