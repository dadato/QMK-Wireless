"""链路层 —— 串口收发 / D14 帧间隔 / ACK 匹配 / 心跳 / 事件自动应答

行为对齐固件侧期望（SK32 角色）：
  - 出帧间隔 >= max(5ms, 本帧线上时间 + 0.5ms)（D14：16B@921600 ≈ 0.17ms + 余量）
  - 每 1s 发 CMD_PING；收到桥的 PING 回 PONG
  - 控制命令按 (cmd, seq) 匹配 ACK/ERR 应答帧
  - 事件帧（0x80..0x87）带 ACK_REQ 时自动回 ACK（seq 取原帧，固件按 seq=0 匹配）
"""
from __future__ import annotations

import queue
import threading
import time
from dataclasses import dataclass
from typing import Callable, List, Optional

from . import proto


class BridgeError(Exception):
    """固件返回 ERR 帧（payload[0] 为 BRIDGE_ECODE_*）。"""

    def __init__(self, code: int, cmd: Optional[int] = None):
        self.code = code
        self.cmd = cmd
        name = proto.ECODE_NAMES.get(code, f"0x{code:02X}")
        super().__init__(f"bridge error {name} (cmd={proto.cmd_name(cmd) if cmd is not None else '-'})")


class BridgeTimeout(Exception):
    pass


# ---------------------------------------------------------------------------
# 传输抽象（真机 = 串口；自测 = 进程内虚拟线）
# ---------------------------------------------------------------------------
class Transport:
    def write(self, data: bytes) -> None:
        raise NotImplementedError

    def read(self, timeout: float) -> bytes:
        raise NotImplementedError

    def close(self) -> None:
        pass


class SerialTransport(Transport):
    """USB-TTL / WCH-Link 虚拟串口（模组 UART1 = PA8 RX / PA9 TX，921600 8N1）

    实测坑（WCH-Link CDC）：采用「小尺寸读 + 短超时」时**会随机丢字节**
    （读请求超时取消时，URB 内剩余数据被一并丢弃）。症状：模块明明在应答，
    上位机却大量 CRC 错/超时。修法：只按当前可读字节数发起读取（必然立即返回，
    不会超时取消），空闲时短轮询；绝不再发“要得比现有字节多”的小读。
    """

    READ_TIMEOUT_S = 0.2      # 仅在“已有字节”的读上使用，实际立即返回
    IDLE_POLL_S = 0.01        # 空闲轮询间隔（≤10ms 收包延迟）

    def __init__(self, port: str, baud: int = 921600):
        import serial  # 延迟导入：自测路径不依赖 pyserial

        self._ser = serial.Serial(port=port, baudrate=baud, bytesize=8,
                                  parity="N", stopbits=1,
                                  timeout=self.READ_TIMEOUT_S, write_timeout=2.0)

    def write(self, data: bytes) -> None:
        self._ser.write(data)

    def read(self, timeout: float) -> bytes:
        n = self._ser.in_waiting
        if n <= 0:
            time.sleep(min(max(timeout, 0.002), self.IDLE_POLL_S))
            n = self._ser.in_waiting
            if n <= 0:
                return b""
        return self._ser.read(n)      # 读“已到字节”，立即返回

    def close(self) -> None:
        self._ser.close()


@dataclass
class Stats:
    tx_frames: int = 0
    rx_frames: int = 0
    crc_errors: int = 0
    ping_rx: int = 0
    pong_rx: int = 0
    events: int = 0
    timeouts: int = 0
    errors_rx: int = 0


class BridgeLink:
    """SK32 链路：线程化 RX + 心跳 + 请求/应答。"""

    def __init__(self, transport: Transport, *, baud: int = 921600,
                 gap_ms: float = 5.0, heartbeat_ms: int = 1000,
                 auto_ack: bool = True,
                 on_event: Optional[Callable[[proto.Frame], None]] = None,
                 on_frame: Optional[Callable[[proto.Frame], None]] = None,
                 log: Optional[Callable[[str], None]] = None):
        self.transport = transport
        self.baud = baud
        self.gap_ms = gap_ms
        self.heartbeat_ms = heartbeat_ms
        self.auto_ack = auto_ack
        self.on_event = on_event
        self.on_frame = on_frame
        self.log = log or (lambda s: None)

        self.stats = Stats()
        self.events: List[proto.Frame] = []
        self.last_pong_ms: Optional[float] = None
        self.last_ping_rx_ms: Optional[float] = None

        self._stop = threading.Event()
        self._tx_lock = threading.Lock()
        self._seq = 0
        self._last_tx = 0.0
        self._pending = {}
        self._pending_lock = threading.Lock()
        self._event_q: "queue.Queue[proto.Frame]" = queue.Queue()
        self._threads: List[threading.Thread] = []
        self._t0 = time.monotonic()

    # ---------------- 时间 / 序号 ----------------
    def now_ms(self) -> float:
        return (time.monotonic() - self._t0) * 1000.0

    def _next_seq(self) -> int:
        with self._tx_lock:
            self._seq = (self._seq + 1) & 0xFF
            return self._seq

    # ---------------- 生命周期 ----------------
    def start(self) -> "BridgeLink":
        for fn in (self._rx_loop, self._hb_loop):
            t = threading.Thread(target=fn, daemon=True)
            t.start()
            self._threads.append(t)
        return self

    def stop(self) -> None:
        self._stop.set()
        for t in self._threads:
            t.join(timeout=1.0)
        self.transport.close()

    def drain(self) -> None:
        """仅读走当前已到达的字节（不等待），用于复位瞬间清残留半帧，避免污染后续解析。"""
        for _ in range(3):
            data = self.transport.read(0.01)
            if not data:
                break

    # ---------------- 发送 ----------------
    def _write_frame(self, cmd: int, seq: int, flags: int, payload: bytes = b"") -> None:
        raw = proto.encode(cmd, seq, flags, payload)
        wire_s = len(raw) * 10.0 / self.baud       # 8N1：10 bit/byte
        need_s = max(self.gap_ms / 1000.0, wire_s + 0.0005)
        with self._tx_lock:
            wait = need_s - (time.monotonic() - self._last_tx)
            if wait > 0:
                time.sleep(wait)
            self.transport.write(raw)
            self._last_tx = time.monotonic()
            self.stats.tx_frames += 1

    def ping(self) -> None:
        """心跳 PING（不等待 PONG，PONG 计数在 RX 线程完成）。"""
        self._write_frame(proto.CMD_PING, self._next_seq(), 0, b"")

    def send_hid(self, cmd: int, payload: bytes) -> None:
        """HID / 透传等无 ACK 快速路径。"""
        self._write_frame(cmd, self._next_seq(), 0, payload)

    def request(self, cmd: int, payload: bytes = b"", timeout: float = 0.6,
                retries: int = 1) -> proto.Frame:
        """控制命令：发一帧并等待 (cmd, seq) 匹配的 ACK / ERR 应答。

        默认 1 次重试：覆盖模块刚复位/深睡唤醒窗口内首帧丢失的情况
        （真机实测：复位后 1~2s 内首条命令偶发超时，重发必中）。
        幂等性例外：FW_DATA/FW_END 等顺序敏感命令由调用方显式传 retries=0。
        """
        seq = self._next_seq()
        q: "queue.Queue[proto.Frame]" = queue.Queue()
        with self._pending_lock:
            self._pending[(cmd, seq)] = q
        try:
            last_err: Optional[Exception] = None
            for _ in range(retries + 1):
                self._write_frame(cmd, seq, 0, payload)
                try:
                    fr = q.get(timeout=timeout)
                except queue.Empty:
                    self.stats.timeouts += 1
                    last_err = BridgeTimeout(
                        f"cmd={proto.cmd_name(cmd)} seq={seq} timeout")
                    continue
                if fr.is_err():
                    raise BridgeError(fr.err_code(), cmd)
                return fr
            raise last_err or BridgeTimeout("request failed")
        finally:
            with self._pending_lock:
                self._pending.pop((cmd, seq), None)

    # ---------------- 接收 ----------------
    def _rx_loop(self) -> None:
        parser = proto.FrameParser(on_crc_error=self._on_crc_error)
        while not self._stop.is_set():
            try:
                data = self.transport.read(0.05)
            except Exception as e:            # 串口拔出等
                if not self._stop.is_set():
                    self.log(f"[link] read error: {e}")
                return
            if not data:
                continue
            for fr in parser.feed(data):
                self._handle(fr)

    def _on_crc_error(self, raw: bytes) -> None:
        self.stats.crc_errors += 1
        self.log(f"[link] crc error: {raw.hex(' ')}")

    def _handle(self, fr: proto.Frame) -> None:
        self.stats.rx_frames += 1
        if self.on_frame:
            self.on_frame(fr)

        # 桥的心跳 PING（flags=0）→ 回 PONG（镜像 SK32 行为，实际固件不依赖它）
        if fr.cmd == proto.CMD_PING and fr.flags == 0:
            self.stats.ping_rx += 1
            self.last_ping_rx_ms = self.now_ms()
            self._write_frame(proto.EVT_PONG, fr.seq, proto.FLAG_ACK, b"")
            return

        # ACK / ERR 应答帧
        if fr.is_ack() or fr.is_err():
            if fr.cmd == proto.EVT_PONG:
                self.stats.pong_rx += 1
                self.last_pong_ms = self.now_ms()
            if fr.is_err():
                self.stats.errors_rx += 1
                self.log(f"[link] ERR {proto.ECODE_NAMES.get(fr.err_code(), fr.err_code)} "
                         f"for {proto.cmd_name(fr.cmd)}")
            with self._pending_lock:
                q = self._pending.get((fr.cmd, fr.seq))
            if q is not None:
                q.put(fr)
            return

        # 事件帧
        if fr.is_event():
            self.stats.events += 1
            self.events.append(fr)
            if len(self.events) > 512:
                del self.events[:256]
            if (fr.flags & proto.FLAG_ACK_REQ) and self.auto_ack:
                self._write_frame(fr.cmd, fr.seq, proto.FLAG_ACK, b"")
            self._event_q.put(fr)
            if self.on_event:
                try:
                    self.on_event(fr)
                except Exception as e:
                    self.log(f"[link] on_event error: {e}")
            return

        self.log(f"[link] unhandled frame: {fr!r}")

    def wait_event(self, timeout: float = 2.0, cmd: Optional[int] = None,
                   pred: Optional[Callable[[proto.Frame], bool]] = None) -> proto.Frame:
        """等待事件帧（跳过不匹配的）。"""
        deadline = time.monotonic() + timeout
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                raise BridgeTimeout(f"event wait timeout cmd={cmd}")
            try:
                fr = self._event_q.get(timeout=remain)
            except queue.Empty:
                raise BridgeTimeout(f"event wait timeout cmd={cmd}")
            if (cmd is None or fr.cmd == cmd) and (pred is None or pred(fr)):
                return fr

    # ---------------- 健康度 ----------------
    def link_ok(self, window_ms: float = 3500.0) -> bool:
        """对端 PING 在窗口内到达即视为链路正常（PONG 用于本端心跳自检）。"""
        if self.last_ping_rx_ms is None:
            return False
        return (self.now_ms() - self.last_ping_rx_ms) <= window_ms

    def heartbeat_loop(self) -> None:
        """单独使用（不 start()）时手动驱动心跳；start() 已内置。"""
        while not self._stop.wait(self.heartbeat_ms / 1000.0):
            self.ping()

    def _hb_loop(self) -> None:
        while not self._stop.wait(self.heartbeat_ms / 1000.0):
            try:
                self.ping()
            except Exception as e:
                if not self._stop.is_set():
                    self.log(f"[link] ping failed: {e}")
