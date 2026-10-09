"""无硬件自测 —— 进程内虚拟串口线 + 模拟桥

验证：CRC 已知答案 / 帧编解码（含逐字节喂入与坏 CRC 重同步）/ 请求-ACK 匹配 /
HID 快速路径 / 心跳 PONG / 事件自动 ACK（无重试）/ ERR 帧 / NV 往返 / 出帧间隔。

运行：python -m sk32sim.selftest
"""
from __future__ import annotations

import sys
import threading
import time
from typing import List, Tuple

from . import proto
from .link import BridgeError, BridgeLink, Transport


# ---------------------------------------------------------------------------
# 虚拟线
# ---------------------------------------------------------------------------
class VirtualWire:
    """双向虚拟串口线：A 端 = 上位机（BridgeLink），B 端 = 模拟桥。"""

    def __init__(self):
        self._cv = threading.Condition()
        self._a2b = bytearray()
        self._b2a = bytearray()
        self.write_log: List[Tuple[float, str, bytes]] = []

    def _push(self, buf: bytearray, d: bytes) -> None:
        with self._cv:
            buf.extend(d)
            self._cv.notify_all()

    def _pop(self, buf: bytearray, timeout: float) -> bytes:
        with self._cv:
            end = time.monotonic() + timeout
            while not buf:
                remain = end - time.monotonic()
                if remain <= 0:
                    return b""
                self._cv.wait(remain)
            out = bytes(buf)
            buf.clear()
            return out

    def write_a(self, d: bytes) -> None:
        self.write_log.append((time.monotonic(), "A", bytes(d)))
        self._push(self._a2b, d)

    def read_a(self, timeout: float) -> bytes:
        return self._pop(self._b2a, timeout)

    def write_b(self, d: bytes) -> None:
        self.write_log.append((time.monotonic(), "B", bytes(d)))
        self._push(self._b2a, d)

    def read_b(self, timeout: float) -> bytes:
        return self._pop(self._a2b, timeout)


class WireTransport(Transport):
    def __init__(self, wire: VirtualWire, side: str):
        self.wire = wire
        self.side = side

    def write(self, data: bytes) -> None:
        (self.wire.write_a if self.side == "A" else self.wire.write_b)(data)

    def read(self, timeout: float) -> bytes:
        return (self.wire.read_a if self.side == "A" else self.wire.read_b)(timeout)

    def close(self) -> None:
        pass


# ---------------------------------------------------------------------------
# 模拟桥（最小实现：只覆盖本自测需要的命令）
# ---------------------------------------------------------------------------
class MockBridge:
    def __init__(self, wire: VirtualWire):
        self.wire = wire
        self.parser = proto.FrameParser()
        self.hid_frames: List[proto.Frame] = []
        self.nv = {}
        self.evt_acks = 0
        self.evt_retries = 0
        self.fw_total = 0
        self.fw_crc_exp = 0
        self.fw_data = bytearray()
        self.fw_done = False
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._evt_pending = False
        self._evt_sent = 0.0

    def start(self) -> None:
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1.0)

    # ---- 收发 ----
    def _reply(self, fr: proto.Frame, payload: bytes = b"") -> None:
        self.wire.write_b(proto.encode(fr.cmd, fr.seq, proto.FLAG_ACK, payload))

    def _error(self, fr: proto.Frame, code: int) -> None:
        self.wire.write_b(proto.encode(fr.cmd, fr.seq, proto.FLAG_ERR, bytes([code])))

    def _send_evt_state(self) -> None:
        self._evt_sent = time.monotonic()
        self._evt_pending = True
        payload = bytes([proto.MODE_2G4, 1, 1, 88])
        self.wire.write_b(proto.encode(proto.EVT_STATE, 0, proto.FLAG_ACK_REQ, payload))

    def _loop(self) -> None:
        while not self._stop.is_set():
            data = self.wire.read_b(0.05)
            if data:
                for fr in self.parser.feed(data):
                    self._handle(fr)
            # 镜像固件：300ms 未收到 ACK 则重传（此处上限 3 次）
            if self._evt_pending and (time.monotonic() - self._evt_sent) > 0.3:
                if self.evt_retries < 3:
                    self.evt_retries += 1
                    self._send_evt_state()
                else:
                    self._evt_pending = False

    def _handle(self, fr: proto.Frame) -> None:
        if fr.is_ack():
            if fr.seq == 0 and self._evt_pending:
                self.evt_acks += 1
                self._evt_pending = False
            return
        if fr.is_err():
            return
        if fr.cmd == proto.CMD_PING:
            self.wire.write_b(proto.encode(proto.EVT_PONG, fr.seq, proto.FLAG_ACK))
        elif fr.cmd == proto.CMD_GET_VERSION:
            self._reply(fr, bytes([0, 1, 0]))
        elif fr.cmd == proto.CMD_GET_STATUS:
            self._reply(fr, bytes([proto.MODE_2G4, 1, 1, 77]))
        elif fr.cmd == proto.CMD_GET_MODE:
            self._reply(fr, bytes([proto.MODE_2G4, 1, proto.RADIO_2G4]))
        elif fr.cmd == proto.CMD_GET_WIRELESS_STATE:
            self._reply(fr, bytes([proto.WL_2G4_LINKED]))
        elif fr.cmd == proto.CMD_SET_HEARTBEAT:
            if len(fr.payload) == 1 and fr.payload[0] <= 1:
                self._reply(fr)
            else:
                self._error(fr, proto.ECODE_PARAM)
        elif fr.cmd == proto.CMD_GET_MAC:
            self._reply(fr, bytes(range(6)))
        elif fr.cmd == proto.CMD_SET_MODE:
            if fr.payload:
                self._reply(fr)
            else:
                self._error(fr, proto.ECODE_PARAM)
        elif fr.cmd == proto.CMD_SET_BATTERY:
            self._reply(fr)
            self._send_evt_state()
        elif fr.cmd == proto.CMD_NV_WRITE:
            if len(fr.payload) == 1 + proto.NV_UNIT:
                self.nv[fr.payload[0]] = fr.payload[1:]
                self._reply(fr)
            else:
                self._error(fr, proto.ECODE_PARAM)
        elif fr.cmd == proto.CMD_NV_READ:
            addr = fr.payload[0] if fr.payload else 0
            self._reply(fr, bytes([addr]) + self.nv.get(addr, bytes(proto.NV_UNIT)))
        elif fr.cmd == proto.CMD_GET_ADC:
            self._reply(fr, bytes([0xE8, 0x03]))          # 1000 mV
        elif fr.cmd == proto.CMD_FW_BEGIN:
            if len(fr.payload) == 6:
                self.fw_total = int.from_bytes(fr.payload[0:4], "little")
                self.fw_crc_exp = int.from_bytes(fr.payload[4:6], "little")
                self.fw_data = bytearray()
                self._reply(fr, bytes([proto.MAX_PAYLOAD - 4]))
            else:
                self._error(fr, proto.ECODE_PARAM)
        elif fr.cmd == proto.CMD_FW_DATA:
            if len(fr.payload) < 5:
                self._error(fr, proto.ECODE_PARAM)
            else:
                off = int.from_bytes(fr.payload[0:4], "little")
                if off != len(self.fw_data):
                    self._error(fr, proto.ECODE_PARAM)
                else:
                    self.fw_data += fr.payload[4:]
                    self._reply(fr)
        elif fr.cmd == proto.CMD_FW_END:
            if (len(self.fw_data) == self.fw_total
                    and proto.crc16_ccitt(bytes(self.fw_data)) == self.fw_crc_exp):
                self.fw_done = True
                self._reply(fr)
            else:
                self._error(fr, proto.ECODE_CRC)
        elif fr.cmd == proto.CMD_FW_ABORT:
            self.fw_data = bytearray()
            self._reply(fr)
        elif fr.cmd in proto.REPORT_LEN:
            self.hid_frames.append(fr)                    # HID 快速路径：无应答
        else:
            self._error(fr, proto.ECODE_PARAM)


# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
_PASS = 0
_FAIL = 0


def check(cond: bool, name: str) -> bool:
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {name}")
    else:
        _FAIL += 1
        print(f"  FAIL  {name}")
    return cond


def main() -> int:
    print("== sk32sim selftest ==")

    # 1. CRC KAT（CRC-16/CCITT-FALSE 标准向量）
    check(proto.crc16_ccitt(b"123456789") == 0x29B1, "CRC-16/CCITT-FALSE KAT = 0x29B1")

    # 2. 帧编解码：逐字节喂入 + 前置垃圾字节 + AA 13 AA 55 干扰
    raw = proto.encode(proto.CMD_HID_KEYBOARD, 7, 0, bytes(range(8)))
    p = proto.FrameParser()
    frames: List[proto.Frame] = []
    for b in b"\x00\xaa\x13\xaa\x55" + raw:
        frames += p.feed(bytes([b]))
    check(len(frames) == 1 and frames[0].cmd == proto.CMD_HID_KEYBOARD
          and frames[0].seq == 7 and frames[0].payload == bytes(range(8)),
          "帧编解码往返（逐字节喂入 + 垃圾前缀）")

    # 3. 坏 CRC → 计数并从下一字节重同步（后续完整帧仍能解出）
    bad = bytearray(raw)
    bad[-1] ^= 0xFF
    p2 = proto.FrameParser()
    got = p2.feed(bytes(bad) + raw)
    check(p2.crc_errors == 1 and len(got) == 1, "坏 CRC 重同步")

    # 4. 链路 + 模拟桥
    wire = VirtualWire()
    link = BridgeLink(WireTransport(wire, "A"), heartbeat_ms=200,
                      log=lambda s: print("   [link]", s))
    mock = MockBridge(wire)
    mock.start()
    link.start()
    time.sleep(0.05)

    r = link.request(proto.CMD_GET_VERSION)
    check(r.payload == bytes([0, 1, 0]), "GET_VERSION 应答")

    r = link.request(proto.CMD_GET_MAC)
    check(r.payload == bytes(range(6)), "GET_MAC 应答")

    r = link.request(proto.CMD_GET_MODE)
    check(r.payload == bytes([proto.MODE_2G4, 1, proto.RADIO_2G4]),
          "GET_MODE 应答（mode/连接/射频）")

    r = link.request(proto.CMD_GET_WIRELESS_STATE)
    check(r.payload == bytes([proto.WL_2G4_LINKED]), "GET_WIRELESS_STATE 应答")

    link.request(proto.CMD_SET_HEARTBEAT, bytes([1]))     # 有效 → ACK，无异常即通过
    try:
        link.request(proto.CMD_SET_HEARTBEAT, bytes([2]))
        check(False, "SET_HEARTBEAT 越界应返回 ERR")
    except BridgeError as e:
        check(e.code == proto.ECODE_PARAM, "SET_HEARTBEAT 越界参数 ERR")

    link.send_hid(proto.CMD_HID_KEYBOARD, bytes([0, 0, 0x04, 0, 0, 0, 0, 0]))
    time.sleep(0.1)
    check(len(mock.hid_frames) == 1
          and mock.hid_frames[0].payload[2] == 0x04, "HID 快速路径（无应答）")

    time.sleep(0.7)
    check(link.stats.pong_rx >= 2, f"心跳 PING→PONG（pong={link.stats.pong_rx}）")

    link.request(proto.CMD_SET_BATTERY, bytes([88]))
    ev = link.wait_event(timeout=0.5, cmd=proto.EVT_STATE)
    check(ev.payload == bytes([proto.MODE_2G4, 1, 1, 88]), "EVT_STATE 载荷解析")
    time.sleep(0.6)
    check(mock.evt_acks == 1 and mock.evt_retries == 0,
          f"事件自动 ACK 且无重试（ack={mock.evt_acks} retry={mock.evt_retries}）")

    try:
        link.request(0x55)
        check(False, "未知命令应返回 ERR")
    except BridgeError as e:
        check(e.code == proto.ECODE_PARAM, "ERR 帧错误码解析")

    link.request(proto.CMD_NV_WRITE, bytes([5]) + bytes(range(32)))
    r = link.request(proto.CMD_NV_READ, bytes([5]))
    check(r.payload[0] == 5 and r.payload[1:] == bytes(range(32)), "NV 写-读往返")

    # 5. 出帧间隔（D14：>= 4ms 宽松判定，固件侧要求 5ms）
    a_ts = [t for (t, side, _) in wire.write_log if side == "A"]
    gaps = [(a_ts[i + 1] - a_ts[i]) * 1000.0 for i in range(len(a_ts) - 1)]
    if gaps:
        check(min(gaps) >= 4.0, f"出帧间隔 >= 4ms（实测最小 {min(gaps):.2f}ms，n={len(gaps)}）")
    else:
        check(False, "出帧间隔（无采样）")

    # 6. BridgeClient / 场景层（复用同一 MockBridge）
    from . import scenarios
    from .client import BridgeClient

    client = BridgeClient(link)
    check(client.get_version() == (0, 1, 0), "BridgeClient.get_version 解析")
    st = client.get_status()
    check(st["mode"] == proto.MODE_2G4 and st["battery"] == 77,
          f"BridgeClient.get_status 解析（{st['mode_name']}/{st['battery']}%）")

    md = client.get_mode()
    check(md["mode"] == proto.MODE_2G4 and md["connected"] and md["radio_name"] == "2G4",
          f"BridgeClient.get_mode 解析（{md['mode_name']}/{md['radio_name']}）")
    ws = client.get_wireless_state()
    check(ws["state"] == proto.WL_2G4_LINKED and ws["name"] == "2G4_LINKED",
          f"BridgeClient.get_wireless_state 解析（{ws['name']}）")
    client.set_heartbeat(True)                            # 便捷封装：无异常即通过
    wire.write_b(proto.encode(proto.EVT_HOST_POWER, 0, 0, bytes([proto.HOST_SUSPEND])))
    ev = client.wait_host_power(timeout=0.5, state=proto.HOST_SUSPEND)
    check(ev.payload == bytes([proto.HOST_SUSPEND]), "EVT_HOST_POWER 解析（挂起）")

    import io
    import contextlib

    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        smoke_failed = scenarios.run_scenario("smoke", client, link)
    check(smoke_failed == 0, f"scenarios.smoke 对 MockBridge 全通过（失败 {smoke_failed}）")

    # 7. OTA 整包下发（flash_file → MockBridge 完整收包 + CRC 校验）
    import os
    import tempfile

    fw = bytes((i * 7) & 0xFF for i in range(133))        # 133B：验证 4B 补齐
    with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as tf:
        tf.write(fw)
        path = tf.name
    total = client.flash_file(path)
    os.unlink(path)
    check(total == 136, f"flash_file 4B 补齐（{total}B）")
    check(mock.fw_done and bytes(mock.fw_data[:len(fw)]) == fw,
          "MockBridge 完整收包且 FW_END CRC 通过")

    link.stop()
    mock.stop()

    print(f"== 结果：{_PASS} passed / {_FAIL} failed ==")
    return 1 if _FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
