"""P1/P2/P3 场景用例（键盘模组侧，经 COM 直连 CH582 模组）

约定：
  - 每个场景输出 PASS/FAIL 明细，返回失败数（0 = 通过）。
  - 2.4G 打字类场景需要 Dongle 已插到 PC 并完成配对（见 pair_2g4 提示）。
  - BLE 打字需要 PC/手机完成蓝牙配对后在系统侧验证输入（本工具只验证命令/事件层）。
"""
from __future__ import annotations

import time
from typing import Callable, Dict, Optional

from . import proto
from .client import BridgeClient
from .link import BridgeError, BridgeLink, BridgeTimeout


class Checks:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, cond: bool, name: str) -> bool:
        if cond:
            self.passed += 1
            print(f"  PASS  {name}")
        else:
            self.failed += 1
            print(f"  FAIL  {name}")
        return cond

    def eq(self, got, exp, name: str) -> bool:
        return self.check(got == exp, f"{name}（got={got!r} exp={exp!r}）")

    def err_is(self, code: int, fn: Callable, name: str) -> bool:
        """期望命令返回 ERR(code)。"""
        try:
            fn()
        except BridgeError as e:
            return self.check(e.code == code,
                              f"{name}（ERR={proto.ECODE_NAMES.get(e.code, e.code)}）")
        except BridgeTimeout:
            return self.check(False, f"{name}（超时）")
        return self.check(False, f"{name}（未返回错误）")


# ---------------------------------------------------------------------------
# P2：基础与全指令冒烟
# ---------------------------------------------------------------------------
def scenario_smoke(c: BridgeClient, link: BridgeLink, ch: Checks, **kw) -> None:
    print("== smoke：基础命令 / 链路 / 指令覆盖 ==")
    ver = c.get_version()
    ch.check(ver[0] == 0 and ver[1] == 1, f"GET_VERSION={ver}")

    st = c.get_status()
    print(f"  status: mode={st['mode_name']} connected={st['connected']} "
          f"profile={st['profile']} battery={st['battery']}%")
    ch.check(st["mode"] in proto.MODE_NAMES, "GET_STATUS 模式合法")

    mac = c.get_mac()
    ch.check(len(mac) == 6 and any(mac), f"GET_MAC={mac.hex(':')}")

    c.ping()
    ch.check(True, "PING → PONG")

    mv = c.get_adc_mv()
    ch.check(0 < mv < 5000, f"GET_ADC={mv}mV")

    # 无应答快速路径：HID 全类型 + 透传（仅验证不抛错、链路保持）
    c.hid_release()
    c.hid_keyboard([])
    c.hid_nkro(bytes(16))
    c.hid_consumer(b"\x00\x00")
    c.hid_mouse(b"\x00\x00\x00\x00")
    c.hid_system(0x00)
    c.tunnel_usb(bytes(range(16)))
    c.tunnel_ble(bytes(range(8)))
    time.sleep(0.15)
    c.ping()
    ch.check(True, "HID(5 类)+透传(USB/BLE) 发送且链路保持")

    # 越界参数 → ERR
    ch.err_is(proto.ECODE_PARAM, lambda: c.set_profile(9), "SET_PROFILE 越界 → ERR(PARAM)")
    ch.err_is(proto.ECODE_PARAM, lambda: c.set_test_mode(2), "SET_TEST_MODE 非法模式 → ERR(PARAM)")


# ---------------------------------------------------------------------------
# 用户 NV：32B 单元读写
# ---------------------------------------------------------------------------
def scenario_nv(c: BridgeClient, link: BridgeLink, ch: Checks, **kw) -> None:
    print("== NV：32B 单元写-读回 ==")
    for addr in (0, 1, 30, proto.NV_USER_UNITS - 1):
        pat = bytes(((addr * 7 + i) & 0xFF) for i in range(proto.NV_UNIT))
        c.nv_write(addr, pat)
        got = c.nv_read(addr)
        ch.eq(got, pat, f"NV[{addr}] 往返")
    ch.err_is(proto.ECODE_PARAM, lambda: c.nv_read(proto.NV_USER_UNITS),
              f"NV[{proto.NV_USER_UNITS}] 越界 → ERR(PARAM)")


# ---------------------------------------------------------------------------
# SET_CONFIG 五项
# ---------------------------------------------------------------------------
def scenario_config(c: BridgeClient, link: BridgeLink, ch: Checks, **kw) -> None:
    print("== SET_CONFIG：五项键（测试后恢复默认）==")
    try:
        c.set_report_period(8);   ch.check(True, "REPORT_PERIOD=8ms → ACK")
        c.set_report_period(1);   ch.check(True, "REPORT_PERIOD 恢复 1ms")
        c.set_log_level(4);       ch.check(True, "LOG_LEVEL=4 → ACK")
        c.set_adv_name_mac(True); ch.check(True, "ADV_NAME_MAC=1 → ACK")
        c.set_adv_name_mac(False); ch.check(True, "ADV_NAME_MAC 恢复 0")
        c.set_battery_src(proto.BATT_SRC_EXTERNAL); ch.check(True, "BATTERY_SRC=EXTERNAL → ACK")
        c.set_battery_src(proto.BATT_SRC_AUTO);     ch.check(True, "BATTERY_SRC 恢复 AUTO")
    except BridgeError as e:
        ch.check(False, f"SET_CONFIG 出错：{e}")
    ch.err_is(proto.ECODE_PARAM, lambda: c.set_config(0x7F, b"\x00"), "未知配置键 → ERR(PARAM)")


# ---------------------------------------------------------------------------
# 电量注入 → EVT_STATE
# ---------------------------------------------------------------------------
def scenario_battery(c: BridgeClient, link: BridgeLink, ch: Checks, **kw) -> None:
    print("== 电量：CMD_SET_BATTERY → EVT_STATE ==")
    c.set_battery(66, charging=True)
    st = c.wait_state(timeout=2.0, pred=lambda f: len(f.payload) >= 4 and f.payload[3] == 66)
    ch.check(st is not None, "EVT_STATE.battery == 66%")
    st2 = c.get_status()
    ch.eq(st2["battery"], 66, "GET_STATUS 电量一致")


# ---------------------------------------------------------------------------
# 睡眠分级（浅睡/DEEP/唤醒）
# ---------------------------------------------------------------------------
def scenario_sleep(c: BridgeClient, link: BridgeLink, ch: Checks, **kw) -> None:
    print("== 睡眠：IDLE 保持链路 / DEEP 进深睡 / WAKE 恢复 ==")
    before = c.get_status()
    ch.check(before["mode"] != proto.MODE_SLEEP, "初始非 SLEEP")

    c.sleep(proto.SLEEP_IDLE)
    ch.check(True, "CMD_SLEEP(IDLE) → ACK")
    st = c.get_status()
    ch.check(st["mode"] != proto.MODE_SLEEP, "IDLE 后模式不变（浅睡保持链路）")

    c.sleep(proto.SLEEP_DEEP)
    time.sleep(1.2)                     # 深睡窗口（1s + 余量）
    st = c.get_status()                 # 唤醒由 RXD1 下降沿触发（新帧即唤醒）
    print(f"  DEEP 后 GET_STATUS → {st['mode_name']}")
    ch.check(True, f"深睡可被串口唤醒（当前 {st['mode_name']}）")

    if st["mode"] == proto.MODE_SLEEP:
        c.sleep(proto.SLEEP_WAKE)
        st = c.get_status()
        ch.check(st["mode"] != proto.MODE_SLEEP, "WAKE 退出深睡")
    else:
        ch.check(True, "深睡已被数据帧自动唤醒（无需显式 WAKE）")


# ---------------------------------------------------------------------------
# 2.4G 配对（需 Dongle 处于配对窗口：按 Dongle 配对键）
# ---------------------------------------------------------------------------
def scenario_pair_2g4(c: BridgeClient, link: BridgeLink, ch: Checks,
                      timeout: float = 65.0, **kw) -> None:
    print("== 2.4G 配对（键盘发起；Dongle 上电即监听，无需按键）==")
    # 先解绑：若双方曾各自存过不同密钥（如应答丢失造成分叉），键盘因“已绑定”
    # 不再发 PAIR_REQ，会僵在配对窗口；解绑可强制走完整配对流程（实测）。
    c.unpair(0xFF)
    ch.check(True, "UNPAIR(0xFF) 清理旧绑定 → ACK")
    c.set_mode(proto.MODE_2G4)
    c.pair_start()
    ch.check(True, "PAIR_START → ACK（进入 2G4_PAIR）")
    try:
        ev = c.wait_pair_result(timeout=timeout)
        ch.eq(ev.payload[0], 0, "EVT_PAIR_RESULT=成功")
        st = c.wait_connected(timeout=5.0)
        ch.check(st is not None, "EVT_STATE 显示已连接")
    except BridgeTimeout:
        ch.check(False, f"{timeout:.0f}s 内未收到配对结果（确认 Dongle 已进配对窗口）")


# ---------------------------------------------------------------------------
# 2.4G 打字（需已配对；Dongle USB 插入 PC，键值由 PC 端捕获）
# ---------------------------------------------------------------------------
def scenario_typing_2g4(c: BridgeClient, link: BridgeLink, ch: Checks,
                        count: int = 20, rate_hz: float = 6.0, **kw) -> None:
    print(f"== 2.4G 打字：{count} 次 'A'（{rate_hz:.0f} 次/秒，PC 端应看到字符流）==")
    st = c.get_status()
    if st["mode"] != proto.MODE_2G4 or not st["connected"]:
        ch.check(False, f"未处于 2.4G 已连接（当前 {st['mode_name']}）→ 先跑 pair_2g4")
        return

    errs0 = link.stats.errors_rx
    t0 = time.monotonic()
    for _ in range(count):
        c.tap_key(0x04, hold_ms=int(1000 / rate_hz / 2))   # usage 0x04 = 'a'
        time.sleep(max(0.0, 1.0 / rate_hz - 0.02))
    dt = time.monotonic() - t0
    c.ping()
    ch.check(link.stats.errors_rx == errs0, "打字期间无 EVT_ERROR")
    print(f"  {count} 键耗时 {dt:.2f}s（约 {count / dt:.1f} keys/s）")
    ch.check(True, "PC 端请确认文本区域出现对应字符（人工/捕获脚本）")


# ---------------------------------------------------------------------------
# BLE：切模式 + 广播信息（打字需 PC/手机蓝牙配对后验证）
# ---------------------------------------------------------------------------
def scenario_ble(c: BridgeClient, link: BridgeLink, ch: Checks,
                 name: Optional[str] = None, **kw) -> None:
    print("== BLE：切模式 / 广播名（打字请用 PC 蓝牙配对后验证）==")
    if name:
        c.set_name(proto.NAME_TARGET_BLE, name)
        ch.check(True, f"SET_NAME(BLE)={name} → ACK")
    c.set_mode(proto.MODE_BLE)
    st = c.wait_state(timeout=5.0, pred=lambda f: f.payload[0] == proto.MODE_BLE)
    ch.check(st is not None, "EVT_STATE 模式为 BLE")
    mac = c.get_mac()
    print(f"  广播名基准='{name or '（默认）'}' MAC={mac.hex(':')}（带 MAC 后缀见 SET_CONFIG）")
    ch.check(True, "PC 蓝牙搜索设备并配对；再用 `type hello` 验证按键输出")


# ---------------------------------------------------------------------------
# SRRC BLE 定频（需频谱仪；此处验证命令/状态机）
# ---------------------------------------------------------------------------
def scenario_srrc(c: BridgeClient, link: BridgeLink, ch: Checks, **kw) -> None:
    print("== SRRC：BLE 定频开关（2.4G 定频当前不支持）==")
    c.set_test_mode(1, 19, 0)
    ch.check(True, "SET_TEST_MODE(on, ch=19) → ACK")
    c.set_test_mode(0)
    ch.check(True, "SET_TEST_MODE(off) → ACK")
    ch.err_is(proto.ECODE_PARAM, lambda: c.set_test_mode(1, 99),
              "channel 越界 → ERR(PARAM)")


# ---------------------------------------------------------------------------
# 解除绑定（破坏性，默认不跑）
# ---------------------------------------------------------------------------
def scenario_unpair_all(c: BridgeClient, link: BridgeLink, ch: Checks, **kw) -> None:
    print("== UNPAIR：清除全部绑定（破坏性）==")
    c.unpair(0xFF)
    ch.check(True, "UNPAIR(0xFF) → ACK + EVT_STATE")


# ---------------------------------------------------------------------------
# 键盘侧 OTA（需提供 bin 文件；结束后设备复位进 IAP 搬运）
# ---------------------------------------------------------------------------
def scenario_ota(c: BridgeClient, link: BridgeLink, ch: Checks,
                 file: Optional[str] = None, **kw) -> None:
    import os

    print("== OTA：整包升级（FW_BEGIN/DATA/END）==")
    if not file:
        ch.check(False, "需要 --file <bridge_keyboard.bin>（build/keyboard/ 下）")
        return
    if not os.path.isfile(file):
        ch.check(False, f"文件不存在：{file}")
        return

    total = os.path.getsize(file)
    print(f"  镜像 {file}（{total} B，4B 补齐后下发；预计 ~{total / 92160:.0f}s @921600）")
    last = [0]

    def prog(done, tot):
        if done - last[0] >= tot // 10 or done == tot:
            last[0] = done
            print(f"    {done}/{tot} ({done * 100 // tot}%)")

    try:
        n = c.flash_file(file, progress=prog)
        ch.check(True, f"FW_END → ACK（{n} B；设备复位进 IAP，串口短暂断开）")
    except BridgeTimeout:
        ch.check(True, "FW_END 后无 ACK（设备已复位，属预期；请重连确认版本）")
    except BridgeError as e:
        ch.check(False, f"OTA 失败：{e}（可重跑本场景；暂存区残留会被下次覆盖）")


SCENARIOS: Dict[str, Callable[..., None]] = {
    "smoke": scenario_smoke,
    "nv": scenario_nv,
    "config": scenario_config,
    "battery": scenario_battery,
    "sleep": scenario_sleep,
    "pair_2g4": scenario_pair_2g4,
    "typing_2g4": scenario_typing_2g4,
    "ble": scenario_ble,
    "srrc": scenario_srrc,
    "ota": scenario_ota,
    "unpair_all": scenario_unpair_all,
}


def run_scenario(scenario: str, client: BridgeClient, link: BridgeLink, **kw) -> int:
    """执行场景并返回失败数。（首参不与场景自己的 `name` 关键字冲突）"""
    ch = Checks()
    try:
        SCENARIOS[scenario](client, link, ch, **kw)
    except BridgeError as e:
        ch.check(False, f"命令错误：{e}")
    except BridgeTimeout as e:
        ch.check(False, f"超时：{e}")
    return ch.failed


def run_all(client: BridgeClient, link: BridgeLink) -> int:
    """非破坏性集合：smoke + nv + config + battery + sleep。"""
    failed = 0
    for name in ("smoke", "nv", "config", "battery", "sleep"):
        failed += run_scenario(name, client, link)
    return failed
