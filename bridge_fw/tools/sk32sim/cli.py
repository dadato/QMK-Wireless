"""命令行入口 —— 经 COM 口（USB-TTL → 模组 PA8/PA9，921600 8N1）驱动 CH582F 桥

用法示例：
  python -m sk32sim.cli list
  python -m sk32sim.cli --port COM7 info
  python -m sk32sim.cli --port COM7 monitor            # 实时打印全部收发帧
  python -m sk32sim.cli --port COM7 cmd get_status
  python -m sk32sim.cli --port COM7 cmd set_mode 3
  python -m sk32sim.cli --port COM7 cmd nv_write 5 00112233445566778899aabbccddeeff...
  python -m sk32sim.cli --port COM7 type "hello world"  # HID 打字（2.4G/BLE 已连接时）
  python -m sk32sim.cli --port COM7 run all             # 非破坏性场景集合
  python -m sk32sim.cli --port COM7 run typing_2g4
"""
from __future__ import annotations

import argparse
import sys
import time
from typing import Dict, Tuple

from . import proto
from .client import BridgeClient
from .link import BridgeError, BridgeLink, BridgeTimeout, SerialTransport
from . import scenarios


# ---------------------------------------------------------------------------
# ASCII → HID usage（US 布局子集；value=(usage, 是否需 Shift)）
# ---------------------------------------------------------------------------
_CHAR_MAP: Dict[str, Tuple[int, bool]] = {}
for i, ch in enumerate("abcdefghijklmnopqrstuvwxyz"):
    _CHAR_MAP[ch] = (0x04 + i, False)
    _CHAR_MAP[ch.upper()] = (0x04 + i, True)
for i, ch in enumerate("123456789"):
    _CHAR_MAP[ch] = (0x1E + i, False)
_CHAR_MAP["0"] = (0x27, False)
for ch, usage in (("!", 0x1E), ("@", 0x1F), ("#", 0x20), ("$", 0x21), ("%", 0x22),
                  ("^", 0x23), ("&", 0x24), ("*", 0x25), ("(", 0x26), (")", 0x27)):
    _CHAR_MAP[ch] = (usage, True)
_CHAR_MAP.update({
    "\n": (0x28, False), "\r": (0x28, False), "\t": (0x2B, False),
    " ": (0x2C, False),  "\b": (0x2A, False), "\x1b": (0x29, False),
    "-": (0x2D, False), "=": (0x2E, False), "[": (0x2F, False), "]": (0x30, False),
    "\\": (0x31, False), ";": (0x33, False), "'": (0x34, False), "`": (0x35, False),
    ",": (0x36, False), ".": (0x37, False), "/": (0x38, False),
    "_": (0x2D, True), "+": (0x2E, True), "{": (0x2F, True), "}": (0x30, True),
    "|": (0x31, True), ":": (0x33, True), '"': (0x34, True), "~": (0x35, True),
    "<": (0x36, True), ">": (0x37, True), "?": (0x38, True),
})
_SHIFT = 0x02


def _print_frame(fr: proto.Frame, link: BridgeLink) -> None:
    kind = ("EVT" if fr.is_event() else
            "ERR" if fr.is_err() else
            "ACK" if fr.is_ack() else "RX ")
    print(f"[{link.now_ms()/1000:9.3f}s] {kind} {proto.cmd_name(fr.cmd):<14} "
          f"seq={fr.seq:<3} flags=0x{fr.flags:02X} {fr.payload.hex(' ')}")


# ---------------------------------------------------------------------------
# cmd 子命令：全命令表
# ---------------------------------------------------------------------------
def do_cmd(client: BridgeClient, name: str, a) -> None:
    f = lambda x: int(x, 0)

    if name == "get_version":
        v = client.get_version()
        print(f"version = {v[0]}.{v[1]}.{v[2]}")
    elif name == "get_status":
        print(client.get_status())
    elif name == "get_mode":
        m = client.get_mode()
        print(f"mode = {m['mode_name']}  connected = {int(m['connected'])}  "
              f"radio = {m['radio_name']}")
    elif name == "get_wireless_state":
        w = client.get_wireless_state()
        print(f"wireless_state = {w['name']}（{w['state']}）")
    elif name == "get_mac":
        print("mac =", client.get_mac().hex(":"))
    elif name == "get_adc":
        print(f"adc = {client.get_adc_mv()} mV")
    elif name == "ping":
        client.ping()
        print("PONG")
    elif name == "set_mode":
        persist = (f(a[1]) != 0) if len(a) > 1 else True
        client.set_mode(f(a[0]), persist)
        print(f"ACK set_mode({proto.MODE_NAMES.get(f(a[0]), a[0])})")
    elif name == "set_profile":
        client.set_profile(f(a[0])); print("ACK")
    elif name == "pair_start":
        client.pair_start(); print("ACK")
    elif name == "unpair":
        client.unpair(f(a[0]) if a else 0xFF); print("ACK + EVT_STATE")
    elif name == "test_mode":
        client.set_test_mode(f(a[0]), f(a[1]) if len(a) > 1 else 0,
                             f(a[2]) if len(a) > 2 else 0)
        print("ACK")
    elif name == "config":
        client.set_config(f(a[0]), bytes.fromhex(a[1]) if len(a) > 1 else b"")
        print("ACK")
    elif name == "report_period":
        client.set_report_period(f(a[0])); print("ACK")
    elif name == "log_level":
        client.set_log_level(f(a[0])); print("ACK")
    elif name == "adv_name_mac":
        client.set_adv_name_mac(f(a[0]) != 0); print("ACK")
    elif name == "conn_interval":
        client.set_conn_interval(f(a[0])); print("ACK")
    elif name == "battery_src":
        client.set_battery_src(f(a[0])); print("ACK")
    elif name == "sleep_policy":
        client.set_sleep_policy(f(a[0])); print("ACK")
    elif name == "set_heartbeat":
        client.set_heartbeat(f(a[0]) != 0); print("ACK")
    elif name == "battery":
        client.set_battery(f(a[0])); print("ACK + EVT_STATE")
    elif name == "sleep":
        client.sleep(f(a[0])); print("ACK")
    elif name == "nv_read":
        print(client.nv_read(f(a[0])).hex(" "))
    elif name == "nv_write":
        client.nv_write(f(a[0]), bytes.fromhex(a[1])); print("ACK")
    elif name == "name":
        target = proto.NAME_TARGET_BLE if a[0].lower() == "ble" else proto.NAME_TARGET_USB
        client.set_name(target, " ".join(a[1:])); print("ACK")
    elif name == "usb_id":
        client.set_usb_id(f(a[0]), f(a[1])); print("ACK（Dongle 侧重枚举）")
    elif name == "reset":
        client.reset(); print("已发送复位（无应答）")
    elif name == "enter_boot":
        client.enter_boot(); print("已发送进常驻 BOOT（写 UART_IAP + 软复位，无应答）")
    elif name == "hid_kb":
        rep = bytes.fromhex(a[0]).ljust(8, b"\x00")
        client.hid_keyboard(keys=rep[2:8], modifier=rep[0])
        print("已发送 HID_KEYBOARD")
    elif name == "hid_nkro":
        client.hid_nkro(bytes.fromhex(a[0])); print("已发送 HID_NKRO")
    elif name == "hid_consumer":
        client.hid_consumer(bytes.fromhex(a[0])); print("已发送 HID_CONSUMER")
    elif name == "hid_mouse":
        client.hid_mouse(bytes.fromhex(a[0])); print("已发送 HID_MOUSE")
    elif name == "hid_system":
        client.hid_system(f(a[0])); print("已发送 HID_SYSTEM")
    elif name == "release":
        client.hid_release(); print("已发送 HID_RELEASE")
    elif name == "tunnel_usb":
        client.tunnel_usb(bytes.fromhex(a[0])); print("已发送 TUNNEL_USB")
    elif name == "tunnel_ble":
        client.tunnel_ble(bytes.fromhex(a[0])); print("已发送 TUNNEL_BLE")
    else:
        print("未知命令。可用："
              "get_version/get_status/get_mode/get_wireless_state/get_mac/get_adc/ping/"
              "set_mode/set_profile/pair_start/unpair/test_mode/config/report_period/"
              "log_level/adv_name_mac/conn_interval/battery_src/sleep_policy/set_heartbeat/"
              "battery/sleep/nv_read/nv_write/name/usb_id/"
              "reset/enter_boot/hid_kb/hid_nkro/hid_consumer/hid_mouse/hid_system/"
              "release/tunnel_usb/tunnel_ble")
        raise SystemExit(2)
    _ = f  # noqa


def do_type(client: BridgeClient, text: str, rate_hz: float) -> None:
    dt = 1.0 / max(1.0, rate_hz)
    for ch in text:
        m = _CHAR_MAP.get(ch)
        if m is None:
            print(f"  [跳过] 不支持的字符 {ch!r}")
            continue
        usage, shift = m
        client.hid_keyboard([usage], _SHIFT if shift else 0)
        time.sleep(max(0.008, dt * 0.4))
        client.hid_keyboard([])
        time.sleep(max(0.008, dt * 0.6))
    print(f"已发送 {len(text)} 个字符（HID 键盘报文）")


# ---------------------------------------------------------------------------
def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="python -m sk32sim.cli",
                                description="SK32 模拟器：COM 口驱动 CH582F 桥")
    p.add_argument("--port", help="串口，如 COM7（list 除外）")
    p.add_argument("--baud", type=int, default=921600)
    sub = p.add_subparsers(dest="action", required=True)

    sub.add_parser("list", help="列出本机串口")
    sub.add_parser("info", help="版本/状态/MAC/ADC 快照")
    sub.add_parser("monitor", help="实时打印全部收发帧（Ctrl+C 退出）")

    sp = sub.add_parser("cmd", help="发送单条命令")
    sp.add_argument("name")
    sp.add_argument("args", nargs="*")

    sp = sub.add_parser("type", help="HID 打字")
    sp.add_argument("text")
    sp.add_argument("--rate", type=float, default=12.0, help="字符/秒（默认 12）")

    sp = sub.add_parser("run", help="运行场景用例")
    sp.add_argument("scenario", help="all | " + " | ".join(scenarios.SCENARIOS))
    sp.add_argument("--name", default=None, help="ble 场景：广播名")
    sp.add_argument("--file", default=None, help="ota 场景：bridge_keyboard.bin 路径")

    sp = sub.add_parser("flash", help="整包 OTA 升级（FW_BEGIN/DATA/END）")
    sp.add_argument("file", help="bridge_keyboard.bin 路径（build/keyboard/ 下）")

    args = p.parse_args(argv)

    if args.action == "list":
        import serial.tools.list_ports as lp
        ports = list(lp.comports())
        if not ports:
            print("（未发现串口）")
        for po in ports:
            print(f"{po.device:<8} {po.description}")
        return 0

    if not args.port:
        p.error("需要 --port（可用 `list` 查看）")

    link = BridgeLink(SerialTransport(args.port, args.baud),
                      on_frame=(lambda fr: _print_frame(fr, link)) if args.action == "monitor" else None,
                      log=lambda s: print(" ", s))
    client = BridgeClient(link)
    link.start()
    print(f"打开 {args.port} @ {args.baud}，心跳中…（Ctrl+C 退出）")
    rc = 0
    try:
        if args.action == "info":
            print(f"version = {client.get_version()}")
            print(f"status  = {client.get_status()}")
            print(f"mac     = {client.get_mac().hex(':')}")
            print(f"adc     = {client.get_adc_mv()} mV")
        elif args.action == "monitor":
            while True:
                time.sleep(0.5)
        elif args.action == "cmd":
            do_cmd(client, args.name, args.args)
        elif args.action == "type":
            do_type(client, args.text, args.rate)
        elif args.action == "flash":
            _last = [0]

            def _prog(d, t, _st=_last):
                if d - _st[0] >= t // 20 or d == t:      # 每 5% 打一行
                    _st[0] = d
                    print(f"  {d}/{t} ({d * 100 // t}%)")

            n = client.flash_file(args.file, progress=_prog)
            print(f"FW_END 已发送（{n} B）→ 设备复位进 IAP 搬运，串口断开属预期")
        elif args.action == "run":
            if args.scenario == "all":
                rc = scenarios.run_all(client, link)
            else:
                if args.scenario not in scenarios.SCENARIOS:
                    p.error(f"未知场景：{args.scenario}")
                rc = scenarios.run_scenario(args.scenario, client, link,
                                            name=args.name, file=args.file)
            print(f"== 场景结果：{'通过' if rc == 0 else f'{rc} 项失败'} ==")
    except (BridgeError, BridgeTimeout) as e:
        print(f"错误：{e}")
        rc = 1
    except KeyboardInterrupt:
        print("\n已停止")
    finally:
        link.stop()
    return rc


if __name__ == "__main__":
    sys.exit(main())
