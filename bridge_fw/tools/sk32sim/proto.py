"""桥协议帧层与常量 —— 与固件 core/ 逐字节一致

帧格式（小端 CRC，CRC 覆盖 LEN..PAYLOAD，来源 core/bridge_proto.h + bridge_uart.c）:

    +--------+-----+-----+-----+-------+---------+----------+
    | SOF    | LEN | CMD | SEQ | FLAGS | PAYLOAD | CRC16    |
    | AA 55  | 1B  | 1B  | 1B  |  1B   |  N B    | 2B（LE） |
    +--------+-----+-----+-----+-------+---------+----------+

CRC-16/CCITT-FALSE：init=0xFFFF, poly=0x1021, 不反转, xorout=0
（对应 core/bridge_crc16.c）。
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import List

# ---------------------------------------------------------------------------
# 帧层常量（bridge_proto.h）
# ---------------------------------------------------------------------------
SOF0 = 0xAA
SOF1 = 0x55
SOF = bytes((SOF0, SOF1))
MAX_PAYLOAD = 64
HDR_LEN = 4
CRC_LEN = 2
FRAME_OVERHEAD = 2 + HDR_LEN + CRC_LEN  # 8

FLAG_ACK_REQ = 0x01
FLAG_ACK = 0x02
FLAG_ERR = 0x04

# ---------------------------------------------------------------------------
# 命令码：SK32 → 桥（bridge_types.h）
# ---------------------------------------------------------------------------
CMD_GET_VERSION = 0x01
CMD_GET_STATUS = 0x02
CMD_SET_MODE = 0x03
CMD_SET_PROFILE = 0x04
CMD_PAIR_START = 0x05
CMD_UNPAIR = 0x06
CMD_RESET = 0x07
CMD_ENTER_BOOT = 0x08
CMD_SET_CONFIG = 0x09
CMD_GET_MAC = 0x0A
CMD_SET_TEST_MODE = 0x0B
CMD_GET_MODE = 0x0C            # 应答 [mode][connected][radio]
CMD_SET_HEARTBEAT = 0x0D       # payload [enable u8]（1=开/0=关）
CMD_GET_WIRELESS_STATE = 0x0E  # 应答 [wl_state u8]（BRIDGE_WL_*）
CMD_GET_BLE_SLOTS = 0x0F       # 应答 [s1][s2][s3][s4][last_active]

CMD_HID_KEYBOARD = 0x10
CMD_HID_NKRO = 0x11
CMD_HID_CONSUMER = 0x12
CMD_HID_MOUSE = 0x13
CMD_HID_SYSTEM = 0x14
CMD_HID_RELEASE = 0x15

CMD_TUNNEL_USB = 0x18
CMD_TUNNEL_BLE = 0x19
CMD_NV_WRITE = 0x1A
CMD_NV_READ = 0x1B
CMD_SET_BATTERY = 0x1C
CMD_SLEEP = 0x1D
CMD_SET_NAME = 0x1F
CMD_SET_USB_ID = 0x20

# 键盘侧 UART OTA（WCH IAP 分区）
CMD_FW_BEGIN = 0x21
CMD_FW_DATA = 0x22
CMD_FW_END = 0x23
CMD_FW_ABORT = 0x24

# 诊断快照（仅排障用，不在市售模组指令集内）：应答 24B
CMD_GET_DIAG = 0x2A

CMD_PING = 0x7F

# ---------------------------------------------------------------------------
# 事件码：桥 → SK32
# ---------------------------------------------------------------------------
EVT_STATE = 0x80
EVT_LEDS = 0x81
EVT_PAIR_RESULT = 0x83
EVT_ERROR = 0x84
EVT_TUNNEL_USB = 0x85
EVT_TUNNEL_BLE = 0x86
EVT_HOST_POWER = 0x87   # payload [state]（BRIDGE_HOST_*）
EVT_PONG = 0x7F

EVENT_NAMES = {
    EVT_STATE: "EVT_STATE",
    EVT_LEDS: "EVT_LEDS",
    EVT_PAIR_RESULT: "EVT_PAIR_RESULT",
    EVT_ERROR: "EVT_ERROR",
    EVT_TUNNEL_USB: "EVT_TUNNEL_USB",
    EVT_TUNNEL_BLE: "EVT_TUNNEL_BLE",
    EVT_HOST_POWER: "EVT_HOST_POWER",
    EVT_PONG: "EVT_PONG",
}

# 事件区间（bridge_types.h：0x80..0x87）
EVT_MIN = 0x80
EVT_MAX = 0x87

# ---------------------------------------------------------------------------
# 参数常量
# ---------------------------------------------------------------------------
# 模式（bridge_mode.h）
MODE_USB = 0
MODE_BLE = 1
MODE_BLE_PAIR = 2
MODE_2G4 = 3
MODE_2G4_PAIR = 4
MODE_OTA = 5
MODE_SLEEP = 6
MODE_NAMES = {
    MODE_USB: "USB",
    MODE_BLE: "BLE",
    MODE_BLE_PAIR: "BLE_PAIR",
    MODE_2G4: "2G4",
    MODE_2G4_PAIR: "2G4_PAIR",
    MODE_OTA: "OTA",
    MODE_SLEEP: "SLEEP",
}

BRIDGE_MAX_BLE_PROFILE = 4

# 名称目标（bridge_types.h）
NAME_TARGET_BLE = 0x00
NAME_TARGET_USB = 0x01

# 电量载荷标志位（CMD_SET_BATTERY 的 flags 字节）
BATT_FLAG_CHARGING = 0x01

# 睡眠分级（bridge_types.h）
SLEEP_WAKE = 0
SLEEP_IDLE = 1
SLEEP_DEEP = 2

# 睡眠策略（bridge_types.h：SET_CONFIG key 0x06）
SLEEP_POLICY_AUTO = 0
SLEEP_POLICY_MANUAL = 1

# 射频通道编号（bridge_types.h：CMD_GET_MODE 应答第三字节 / port_radio_* 的 mode）
RADIO_NONE = 0
RADIO_BLE = 1
RADIO_2G4 = 2
RADIO_NAMES = {RADIO_NONE: "NONE", RADIO_BLE: "BLE", RADIO_2G4: "2G4"}

# 主机电源状态（bridge_types.h：EVT_HOST_POWER / BRIDGE_RADIO_PKT_HOST）
HOST_AWAKE = 0
HOST_SUSPEND = 1
HOST_NAMES = {HOST_AWAKE: "AWAKE", HOST_SUSPEND: "SUSPEND"}

# 无线综合状态（bridge_types.h：CMD_GET_WIRELESS_STATE 应答单字节）
WL_USB = 0
WL_IDLE = 1
WL_BLE_PAIRING = 2
WL_2G4_PAIRING = 3
WL_BLE_LINKED = 4
WL_2G4_LINKED = 5
WL_BLE_DISCONN = 6
WL_2G4_DISCONN = 7
WL_OTA = 8
WL_SLEEP = 9
WL_NAMES = {
    WL_USB: "USB",
    WL_IDLE: "IDLE",
    WL_BLE_PAIRING: "BLE_PAIRING",
    WL_2G4_PAIRING: "2G4_PAIRING",
    WL_BLE_LINKED: "BLE_LINKED",
    WL_2G4_LINKED: "2G4_LINKED",
    WL_BLE_DISCONN: "BLE_DISCONN",
    WL_2G4_DISCONN: "2G4_DISCONN",
    WL_OTA: "OTA",
    WL_SLEEP: "SLEEP",
}

# SET_CONFIG 键（bridge_cfg.h）
CFG_KEY_REPORT_PERIOD = 0x01   # value: u16 LE(ms)
CFG_KEY_LOG_LEVEL = 0x02       # value: u8
CFG_KEY_ADV_NAME_MAC = 0x03    # value: u8
CFG_KEY_CONN_INTERVAL = 0x04   # value: u8（单位 1.25ms）
CFG_KEY_SLEEP_POLICY = 0x06    # value: u8（0=AUTO / 1=MANUAL）
CFG_KEY_NAMES = {
    CFG_KEY_REPORT_PERIOD: "REPORT_PERIOD",
    CFG_KEY_LOG_LEVEL: "LOG_LEVEL",
    CFG_KEY_ADV_NAME_MAC: "ADV_NAME_MAC",
    CFG_KEY_CONN_INTERVAL: "CONN_INTERVAL",
    CFG_KEY_SLEEP_POLICY: "SLEEP_POLICY",
}

# 透传（bridge_types.h + bridge_config.h）
TUNNEL_USB = 0x01
TUNNEL_BLE = 0x02
TUNNEL_USB_LEN = 64
TUNNEL_BLE_LEN = 32

# HID 报文长度（bridge_types.h）
REPORT_LEN = {
    CMD_HID_KEYBOARD: 8,
    CMD_HID_NKRO: 16,
    CMD_HID_CONSUMER: 2,
    CMD_HID_MOUSE: 4,
    CMD_HID_SYSTEM: 1,
    CMD_HID_RELEASE: 0,
}

# 用户 NV（bridge_config.h「应用 NV 全槽分配」：64 槽 × 64B）
#   1..3=配置 / 4=多主机索引表 / 5..24=预留 / 25..47=用户 / 48..63=BLE SNV
NV_UNIT = 32
NV_USER_BASE = 25
NV_SNV_LO = 48
NV_USER_UNITS = (NV_SNV_LO - NV_USER_BASE) * (64 // NV_UNIT)  # 46 单元 = 1472B

# 定频测试（bridge_test.h）
TEST_CH_MAX = 39
TEST_POWER_MAX = 3

# 错误码 EVT_ERROR / ERR 帧（bridge_types.h）
ECODE_CRC = 0x01
ECODE_UART_LOST = 0x02
ECODE_RADIO = 0x03
ECODE_PAIR = 0x04
ECODE_ACK_TIMEOUT = 0x05
ECODE_PARAM = 0x06
ECODE_STATE = 0x07
ECODE_ROLLOVER = 0x08
ECODE_NAMES = {
    ECODE_CRC: "CRC",
    ECODE_UART_LOST: "UART_LOST",
    ECODE_RADIO: "RADIO",
    ECODE_PAIR: "PAIR",
    ECODE_ACK_TIMEOUT: "ACK_TIMEOUT",
    ECODE_PARAM: "PARAM",
    ECODE_STATE: "STATE",
    ECODE_ROLLOVER: "ROLLOVER",
}

# 命令名（打印用）
CMD_NAMES = {
    CMD_GET_VERSION: "GET_VERSION",
    CMD_GET_STATUS: "GET_STATUS",
    CMD_SET_MODE: "SET_MODE",
    CMD_SET_PROFILE: "SET_PROFILE",
    CMD_PAIR_START: "PAIR_START",
    CMD_UNPAIR: "UNPAIR",
    CMD_RESET: "RESET",
    CMD_ENTER_BOOT: "ENTER_BOOT",
    CMD_SET_CONFIG: "SET_CONFIG",
    CMD_GET_MAC: "GET_MAC",
    CMD_SET_TEST_MODE: "SET_TEST_MODE",
    CMD_GET_MODE: "GET_MODE",
    CMD_SET_HEARTBEAT: "SET_HEARTBEAT",
    CMD_GET_WIRELESS_STATE: "GET_WIRELESS_STATE",
    CMD_GET_BLE_SLOTS: "GET_BLE_SLOTS",
    CMD_HID_KEYBOARD: "HID_KEYBOARD",
    CMD_HID_NKRO: "HID_NKRO",
    CMD_HID_CONSUMER: "HID_CONSUMER",
    CMD_HID_MOUSE: "HID_MOUSE",
    CMD_HID_SYSTEM: "HID_SYSTEM",
    CMD_HID_RELEASE: "HID_RELEASE",
    CMD_TUNNEL_USB: "TUNNEL_USB",
    CMD_TUNNEL_BLE: "TUNNEL_BLE",
    CMD_NV_WRITE: "NV_WRITE",
    CMD_NV_READ: "NV_READ",
    CMD_SET_BATTERY: "SET_BATTERY",
    CMD_SLEEP: "SLEEP",
    CMD_SET_NAME: "SET_NAME",
    CMD_SET_USB_ID: "SET_USB_ID",
    CMD_FW_BEGIN: "FW_BEGIN",
    CMD_FW_DATA: "FW_DATA",
    CMD_FW_END: "FW_END",
    CMD_FW_ABORT: "FW_ABORT",
    CMD_GET_DIAG: "GET_DIAG",
    CMD_PING: "PING",
}


def cmd_name(cmd: int) -> str:
    return CMD_NAMES.get(cmd, EVENT_NAMES.get(cmd, f"0x{cmd:02X}"))


def wl_name(state: int) -> str:
    return WL_NAMES.get(state, f"?{state}")


def radio_name(radio: int) -> str:
    return RADIO_NAMES.get(radio, f"?{radio}")


def host_name(state: int) -> str:
    return HOST_NAMES.get(state, f"?{state}")


# ---------------------------------------------------------------------------
# CRC-16/CCITT-FALSE（与 core/bridge_crc16.c 同参数）
# ---------------------------------------------------------------------------
def _build_table() -> List[int]:
    table = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if (c & 0x8000) else (c << 1)
            c &= 0xFFFF
        table.append(c)
    return table


_CRC_TABLE = _build_table()


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _CRC_TABLE[((crc >> 8) ^ b) & 0xFF]
    return crc


# ---------------------------------------------------------------------------
# 帧编解码
# ---------------------------------------------------------------------------
@dataclass
class Frame:
    cmd: int
    seq: int
    flags: int
    payload: bytes = b""

    def is_ack(self) -> bool:
        return bool(self.flags & FLAG_ACK)

    def is_err(self) -> bool:
        return bool(self.flags & FLAG_ERR)

    def is_event(self) -> bool:
        """0x80..0x87 的事件帧（与固件一致，均带 ACK_REQ）。"""
        return EVT_MIN <= self.cmd <= EVT_MAX

    def err_code(self) -> int:
        return self.payload[0] if self.payload else 0

    def __repr__(self) -> str:
        return (f"Frame(cmd=0x{self.cmd:02X}/{cmd_name(self.cmd)}, seq={self.seq}, "
                f"flags=0x{self.flags:02X}, payload={self.payload.hex(' ')})")


def encode(cmd: int, seq: int, flags: int, payload: bytes = b"") -> bytes:
    """按 bridge_uart_encode() 组帧。payload 长度超限抛 ValueError。"""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload {len(payload)}B > MAX_PAYLOAD {MAX_PAYLOAD}")
    hdr = bytes((len(payload), cmd & 0xFF, seq & 0xFF, flags & 0xFF))
    crc = crc16_ccitt(hdr + payload)
    return SOF + hdr + payload + struct.pack("<H", crc)


class FrameParser:
    """增量帧解析器（镜像 bridge_uart.c 的 RX 状态机容错行为：

    - 合法 SOF 序列 AA 55（兼容 AA AA 55）
    - LEN > 64 即丢弃重同步
    - CRC 不符则从下一个字节重新搜 SOF
    - 跨 read 边界的半个 SOF 会被保留
    """

    def __init__(self, on_crc_error=None):
        self._buf = bytearray()
        self._on_crc_error = on_crc_error
        self.crc_errors = 0
        self.frames = 0

    def feed(self, data: bytes) -> List[Frame]:
        out: List[Frame] = []
        self._buf.extend(data)
        while True:
            i = self._buf.find(SOF)
            if i < 0:
                # 保留可能的半个 SOF（尾部单字节 AA）
                if self._buf and self._buf[-1] == SOF0:
                    self._buf = self._buf[-1:]
                else:
                    self._buf.clear()
                return out
            if i > 0:
                del self._buf[:i]
            if len(self._buf) < 2 + HDR_LEN:
                return out
            ln = self._buf[2]
            if ln > MAX_PAYLOAD:
                del self._buf[0]          # 重同步
                continue
            total = FRAME_OVERHEAD + ln
            if len(self._buf) < total:
                return out
            body = bytes(self._buf[2:2 + HDR_LEN + ln])
            calc = crc16_ccitt(body)
            rx_crc = self._buf[total - 2] | (self._buf[total - 1] << 8)
            if calc == rx_crc:
                out.append(Frame(cmd=body[1], seq=body[2], flags=body[3],
                                 payload=body[4:]))
                self.frames += 1
                del self._buf[:total]
            else:
                self.crc_errors += 1
                if self._on_crc_error:
                    self._on_crc_error(bytes(self._buf[:total]))
                del self._buf[0]          # 重同步
