"""SK32 客户端 —— 全命令表 → Python 方法（与 core/bridge_proto.c 一一对应）

命令语义：
  - 控制类（GET_*/SET_*/PAIR/UNPAIR/NV/BATTERY/SLEEP/ADC/NAME/USB_ID/PING）：
    发帧后等待固件 ACK（reply，payload 视命令而定）或 ERR 帧 → 失败抛 BridgeError。
  - 快速路径（HID 0x10~0x15 / TUNNEL 0x18/0x19 / RESET / ENTER_BOOT）：无 ACK。
"""
from __future__ import annotations

import time
from typing import Optional, Sequence, Union

from . import proto
from .link import BridgeError, BridgeLink, BridgeTimeout


class BridgeClient:
    def __init__(self, link: BridgeLink):
        self.link = link

    # ---------------- 基础 ----------------
    def get_version(self):
        p = self.link.request(proto.CMD_GET_VERSION).payload
        return (p[0], p[1], p[2]) if len(p) >= 3 else (0, 0, 0)

    def get_status(self) -> dict:
        p = self.link.request(proto.CMD_GET_STATUS).payload
        return {
            "mode": p[0],
            "mode_name": proto.MODE_NAMES.get(p[0], f"?{p[0]}"),
            "connected": bool(p[1]),
            "profile": p[2],
            "battery": p[3],
            "sleep_policy": p[4] if len(p) >= 5 else None,
        }

    def get_mode(self) -> dict:
        """轻量模式查询（0x0C）：应答 [mode][connected][radio]，不含档位/电量。"""
        p = self.link.request(proto.CMD_GET_MODE).payload
        mode = p[0] if len(p) >= 1 else 0
        radio = p[2] if len(p) >= 3 else 0
        return {
            "mode": mode,
            "mode_name": proto.MODE_NAMES.get(mode, f"?{mode}"),
            "connected": bool(p[1]) if len(p) >= 2 else False,
            "radio": radio,
            "radio_name": proto.radio_name(radio),
        }

    def get_wireless_state(self) -> dict:
        """无线综合状态（0x0E）：应答 [wl_state]（proto.WL_*），免组合判断。"""
        p = self.link.request(proto.CMD_GET_WIRELESS_STATE).payload
        state = p[0] if p else 0
        return {"state": state, "name": proto.wl_name(state)}

    def get_mac(self) -> bytes:
        return self.link.request(proto.CMD_GET_MAC).payload

    def ping(self, timeout: float = 0.5):
        return self.link.request(proto.CMD_PING, timeout=timeout)

    # ---------------- 控制 ----------------
    def set_mode(self, mode: int, persist: bool = True) -> None:
        self.link.request(proto.CMD_SET_MODE, bytes([mode, 1 if persist else 0]))

    def set_profile(self, profile: int) -> None:
        self.link.request(proto.CMD_SET_PROFILE, bytes([profile]))

    def pair_start(self) -> None:
        self.link.request(proto.CMD_PAIR_START)

    def unpair(self, profile: int = 0xFF) -> None:
        self.link.request(proto.CMD_UNPAIR, bytes([profile]))

    def set_test_mode(self, mode: int, channel: int = 0, power: int = 0) -> None:
        self.link.request(proto.CMD_SET_TEST_MODE, bytes([mode, channel, power]))

    def set_config(self, key: int, value: bytes) -> None:
        self.link.request(proto.CMD_SET_CONFIG, bytes([key]) + bytes(value))

    # SET_CONFIG 便捷封装（键定义见 core/bridge_cfg.h）
    def set_report_period(self, ms: int) -> None:
        self.set_config(proto.CFG_KEY_REPORT_PERIOD, int(ms).to_bytes(2, "little"))

    def set_log_level(self, level: int) -> None:
        self.set_config(proto.CFG_KEY_LOG_LEVEL, bytes([level]))

    def set_adv_name_mac(self, en: bool) -> None:
        self.set_config(proto.CFG_KEY_ADV_NAME_MAC, bytes([1 if en else 0]))

    def set_conn_interval(self, units_1p25ms: int) -> None:
        self.set_config(proto.CFG_KEY_CONN_INTERVAL, bytes([units_1p25ms]))

    def set_battery_src(self, src: int) -> None:
        self.set_config(proto.CFG_KEY_BATTERY_SRC, bytes([src]))

    def set_sleep_policy(self, policy: int) -> None:
        """SET_CONFIG key 0x06：0=AUTO（自动休眠）/ 1=MANUAL（仅指令休眠）。"""
        self.set_config(proto.CFG_KEY_SLEEP_POLICY, bytes([policy]))

    def set_heartbeat(self, enable: bool) -> None:
        """心跳周期开关（0x0D）：关闭后不发 PING、不做失联判定、不回 PONG。"""
        self.link.request(proto.CMD_SET_HEARTBEAT, bytes([1 if enable else 0]))

    def get_adc_mv(self) -> int:
        p = self.link.request(proto.CMD_GET_ADC).payload
        return p[0] | (p[1] << 8)

    def set_battery(self, percent: int, charging: bool = False) -> None:
        """下发电量（0x1C）：[pct u8][flags u8]，flags bit0 = 充电中。"""
        flags = proto.BATT_FLAG_CHARGING if charging else 0
        self.link.request(proto.CMD_SET_BATTERY, bytes([percent, flags]))

    def sleep(self, level: int) -> None:
        self.link.request(proto.CMD_SLEEP, bytes([level]))

    def nv_write(self, addr: int, data: bytes) -> None:
        if len(data) != proto.NV_UNIT:
            raise ValueError(f"NV 单元必须 {proto.NV_UNIT}B，当前 {len(data)}B")
        self.link.request(proto.CMD_NV_WRITE, bytes([addr]) + bytes(data))

    def nv_read(self, addr: int) -> bytes:
        p = self.link.request(proto.CMD_NV_READ, bytes([addr])).payload
        return p[1:]

    def set_name(self, target: int, name: Union[str, bytes]) -> None:
        data = name.encode("ascii", "ignore") if isinstance(name, str) else bytes(name)
        self.link.request(proto.CMD_SET_NAME, bytes([target]) + data)

    def set_usb_id(self, vid: int, pid: int) -> None:
        self.link.request(proto.CMD_SET_USB_ID,
                          vid.to_bytes(2, "little") + pid.to_bytes(2, "little"))

    def reset(self) -> None:
        self.link.send_hid(proto.CMD_RESET, b"")          # 无应答（立即复位）

    def enter_boot(self) -> None:
        self.link.send_hid(proto.CMD_ENTER_BOOT, b"")     # 无应答（写 UART_IAP + 软复位，进常驻 BOOT）

    # ---------------- HID 快速路径 ----------------
    def hid_keyboard(self, keys: Sequence[int] = (), modifier: int = 0) -> None:
        payload = bytes([modifier & 0xFF, 0]) + bytes(keys)[:6].ljust(6, b"\x00")
        self.link.send_hid(proto.CMD_HID_KEYBOARD, payload)

    def tap_key(self, usage: int, modifier: int = 0, hold_ms: int = 15) -> None:
        self.hid_keyboard([usage], modifier)
        time.sleep(hold_ms / 1000.0)
        self.hid_keyboard([])

    def hid_nkro(self, bitmap: bytes) -> None:
        self.link.send_hid(proto.CMD_HID_NKRO, bytes(bitmap)[:16].ljust(16, b"\x00"))

    def hid_consumer(self, usage: Sequence[int]) -> None:
        self.link.send_hid(proto.CMD_HID_CONSUMER, bytes(usage)[:2].ljust(2, b"\x00"))

    def hid_mouse(self, report: bytes) -> None:
        self.link.send_hid(proto.CMD_HID_MOUSE, bytes(report)[:4].ljust(4, b"\x00"))

    def hid_system(self, code: int) -> None:
        self.link.send_hid(proto.CMD_HID_SYSTEM, bytes([code & 0xFF]))

    def hid_release(self) -> None:
        self.link.send_hid(proto.CMD_HID_RELEASE, b"")

    # ---------------- 透传 ----------------
    def tunnel_usb(self, data: bytes) -> None:
        self.link.send_hid(proto.CMD_TUNNEL_USB, bytes(data)[:proto.TUNNEL_USB_LEN])

    def tunnel_ble(self, data: bytes) -> None:
        self.link.send_hid(proto.CMD_TUNNEL_BLE, bytes(data)[:proto.TUNNEL_BLE_LEN])

    # ---------------- 固件升级（键盘侧 UART OTA，WCH IAP 分区） ----------------
    def fw_begin(self, total_len: int, crc: int) -> int:
        """FW_BEGIN → 返回固件答复的 chunk_max（一般为 60）。"""
        p = int(total_len).to_bytes(4, "little") + int(crc & 0xFFFF).to_bytes(2, "little")
        r = self.link.request(proto.CMD_FW_BEGIN, p, timeout=1.0, retries=0)
        return r.payload[0] if r.payload else 0

    def fw_data(self, offset: int, data: bytes) -> None:
        # 固件对“已写入区间”的重复帧幂等（ACK 丢失可安全重传）。
        # WCH-Link CDC 高速传输随机丢 ACK（插桩实测丢包率~8.5%，字节完好），
        # retries=6 使连丢概率降到 ~2e-6，避免整包中途超时失败。
        self.link.request(proto.CMD_FW_DATA,
                          int(offset).to_bytes(4, "little") + bytes(data),
                          timeout=2.0, retries=6)

    def fw_end(self) -> None:
        self.link.request(proto.CMD_FW_END, timeout=1.5, retries=0)

    def fw_abort(self) -> None:
        self.link.request(proto.CMD_FW_ABORT, timeout=1.0, retries=0)

    def flash_file(self, path: str, chunk: int = 0, progress=None) -> int:
        """整包升级：读 bin → A方案先跳BOOT → 4B 补齐（0xFF）→ FW_BEGIN/DATA/END。

        跳 BOOT：发送 CMD_ENTER_BOOT（App 写 UART_IAP 复位，BOOT 驻留 UART 收包），
        短暂等待后 FW_BEGIN 由 BOOT 应答即证明已进 BOOT。
        注意：FW_END 后 BOOT 写 IAP 搬运标志并复位搬 Stage→APP_A，串口会短暂断开。"""
        with open(path, "rb") as fp:
            data = fp.read()
        # A 方案：先进常驻 BOOT 收包模式（无应答，立即复位进 BOOT）
        self.link.send_hid(proto.CMD_ENTER_BOOT, b"")
        time.sleep(0.4)
        # 清掉复位瞬间残留字节（BOOT 重新初始化 UART1 前可能吐出半帧）
        self.link.drain()
        pad = (-len(data)) % 4
        if pad:
            data += b"\xFF" * pad
        total = len(data)
        crc = proto.crc16_ccitt(data)

        cm = self.fw_begin(total, crc)
        if cm == 0:
            raise BridgeError(proto.ECODE_STATE, proto.CMD_FW_BEGIN)

        n = chunk or min(cm, proto.MAX_PAYLOAD - 4)
        n -= n % 4
        if n <= 0:
            n = 4
        for off in range(0, total, n):
            self.fw_data(off, data[off:off + n])
            if progress:
                progress(off + n, total)
        try:
            self.fw_end()
        except BridgeTimeout:
            # FW_END 已生效但 ACK 丢失时设备立即复位（搬运中）→ 无 ACK 属预期
            pass
        return total

    # ---------------- 状态等待 ----------------
    def wait_state(self, timeout: float = 5.0, pred=None):
        return self.link.wait_event(timeout=timeout, cmd=proto.EVT_STATE, pred=pred)

    def wait_pair_result(self, timeout: float = 65.0):
        return self.link.wait_event(timeout=timeout, cmd=proto.EVT_PAIR_RESULT)

    def wait_host_power(self, timeout: float = 5.0, state=None):
        """等待 EVT_HOST_POWER（主机挂起/恢复）；state 给定则匹配该状态（proto.HOST_*）。"""
        pred = None
        if state is not None:
            pred = lambda f: len(f.payload) >= 1 and f.payload[0] == state
        return self.link.wait_event(timeout=timeout, cmd=proto.EVT_HOST_POWER, pred=pred)

    def wait_connected(self, timeout: float = 30.0):
        """等待 EVT_STATE 里 connected=1。"""
        return self.wait_state(timeout=timeout,
                               pred=lambda f: len(f.payload) >= 2 and f.payload[1] == 1)
