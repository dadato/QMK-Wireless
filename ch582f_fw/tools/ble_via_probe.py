"""VIA 无线通路验证（hidapi 直连 Windows HID 集合，路径与 WebHID / usevia.app 完全一致）。

两条通路（对齐 SPEC §5.8）：
  通路 A｜2.4G —— 写 Dongle 复合 HID IF2 的 vendor 集合（Report ID 0，32B）
                -> Dongle 经 2.4G -> 键盘模组 -> UART EVT_TUNNEL_USB(0x85)；
                UART 回 CMD_TUNNEL_USB(0x18)(32B) -> 模组 -> 2.4G -> Dongle vendor IN。
  通路 B｜BLE  —— 写键盘模组 HOGP vendor 集合（Report ID 5，32B，Usage Page 0xFF60/0x61）
                -> 模组 UART EVT_TUNNEL_BLE(0x86)；
                UART 回 CMD_TUNNEL_BLE(0x19)(32B) -> 模组 -> HOGP vendor IN Notify。

前提：对应无线链路已连接（通路 B 需先在系统蓝牙中配对；通路 A 需键盘处于 2G4 且与
      Dongle 已配对）。UART 侧由本脚本扮演 SK32（自动 ACK / 心跳 / 事件应答）。

用法：
  # 全自动双通路（推荐）：单进程内 唤醒→MANUAL→BLE→2.4G→还原，最后打印汇总表
  python tools/ble_via_probe.py --via both --port COM10 --baud 921600

  # 单通路（前置状态需自行准备好，见 SKILL §12.9）
  python tools/ble_via_probe.py --via ble --port COM10 --baud 921600
  python tools/ble_via_probe.py --via 2g4 --port COM10 --baud 921600

退出码：0=全部 PASS / 1=有 FAIL / 2=环境或链路前置不满足。
"""
from __future__ import annotations

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import hid  # noqa: E402

from sk32sim import proto  # noqa: E402
from sk32sim.client import BridgeClient  # noqa: E402
from sk32sim.link import BridgeLink, SerialTransport  # noqa: E402

TX = bytes([0x01, 0x00, 0x0D]) + bytes(29)     # 主机 -> 键盘（VIA id=1 ver=0x000D）
RX = bytes([0x01, 0x00, 0x0D]) + bytes(29)     # 键盘 -> 主机应答

# 通路配置：HID 路径筛选子串 / Report ID / 下行事件 / 上行命令 / 展示名
VIA_PROFILES = {
    "ble": {
        "label": "BLE (HOGP RID5)",
        "path_hint": "00001812",
        "rid": 5,
        "evt": proto.EVT_TUNNEL_BLE,
        "cmd": proto.CMD_TUNNEL_BLE,
    },
    "2g4": {
        "label": "2.4G (Dongle RID0)",
        "path_hint": "mi_02",
        "rid": 0,
        "evt": proto.EVT_TUNNEL_USB,
        "cmd": proto.CMD_TUNNEL_USB,
    },
}


def find_hid(path_hint: str) -> dict | None:
    for d in hid.enumerate():
        path = d["path"].decode(errors="replace")
        if d.get("usage_page") == 0xFF60 and path_hint in path.lower():
            return d
    return None


def hid_roundtrip(prof: dict, link: BridgeLink, settle_s: float = 2.0) -> dict:
    """在已启动的 link 上跑一次该通路的 32B 往返。

    返回 {"down","up","evt_n","hid_in"}；down=模组收到主机出帧，up=主机收到模组通知。
    """
    evt = []

    def on_event(fr):
        if fr.cmd == prof["evt"]:
            evt.append(fr.payload)
            print(f"  [uart] {proto.cmd_name(fr.cmd)} <- {fr.payload.hex(' ')}")
            link.send_hid(prof["cmd"], RX)
            print(f"  [uart] {proto.cmd_name(prof['cmd'])} -> {RX.hex(' ')}")
        elif fr.cmd == proto.EVT_STATE:
            print(f"  [uart] EVT_STATE      <- {fr.payload.hex(' ')}")

    link.on_event = on_event

    dev = find_hid(prof["path_hint"])
    if dev is None:
        print(f"!! 未找到 {prof['label']} 的 vendor HID 集合（Usage Page 0xFF60）。")
        return {"down": False, "up": False, "evt_n": 0, "hid_in": None}
    print(f"HID 集合: {dev['path'].decode(errors='replace')}")

    h = hid.device()
    try:
        h.open_path(dev["path"])
    except Exception as e:  # noqa: BLE001
        print(f"!! 打开 HID 集合失败：{e}")
        return {"down": False, "up": False, "evt_n": 0, "hid_in": None}

    got = None
    try:
        while h.read(64, 10):     # 清残留 IN 报文
            pass

        for _ in range(2):
            n = h.write(bytes([prof["rid"]]) + TX)
            print(f"  [hid ] write RID{prof['rid']} OUT ({n}B) -> {TX.hex(' ')}")

            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline:
                data = h.read(64, 200)
                if not data:
                    continue
                raw = bytes(data)
                print(f"  [hid ] read  ({len(raw)}B) <- {raw.hex(' ')}")
                # RID0（Dongle 单报文）Windows 不带 report ID 前缀；其余带 1B 前缀
                if len(raw) >= 33 and raw[0] == prof["rid"]:
                    got = raw[1:33]
                    break
                if prof["rid"] == 0 and len(raw) == 32:
                    got = raw[:32]
                    break
            if got is not None:
                break
            print(f"  上行未回，重试一次（{prof['label']} 首次打开 IN 偶发丢首帧）…")

        print(f"保持 {settle_s:.0f}s 观察链路是否稳定（有无断开/重连抖动）…")
        time.sleep(settle_s)
    finally:
        h.close()

    ok_down = bool(evt) and evt[-1][:32] == TX
    ok_up = got is not None and got[:32] == RX
    return {"down": ok_down, "up": ok_up, "evt_n": len(evt), "hid_in": got}


def print_result(prof: dict, r: dict) -> None:
    print(f"UART {proto.cmd_name(prof['evt'])} 次数 = {r['evt_n']}")
    if r.get("hid_in") is not None:
        print(f"  HID IN payload = {r['hid_in'].hex(' ')}")
    print(f"下行(主机 -> 键盘) 出帧 : {'PASS' if r['down'] else 'FAIL'}")
    print(f"上行(键盘 -> 主机) 通知 : {'PASS' if r['up'] else 'FAIL'}")


# ---------------------------------------------------------------------------
# 编排辅助（both 模式用；深睡唤醒 / 睡眠策略 / 模式切换 / 状态等待）
# ---------------------------------------------------------------------------
def _wake(client: BridgeClient, retries: int = 8) -> bool:
    """深睡态（MODE_SLEEP=6）对普通命令不响应，须先 SLEEP_WAKE；单次可能丢首帧故重试。"""
    for _ in range(retries):
        try:
            client.sleep(proto.SLEEP_WAKE)
            return True
        except Exception:  # noqa: BLE001
            time.sleep(0.3)
    return False


def _set_mode(client: BridgeClient, mode: int, retries: int = 4) -> bool:
    for _ in range(retries):
        try:
            client.set_mode(mode)
            return True
        except Exception as e:  # noqa: BLE001
            print(f"  set_mode {proto.MODE_NAMES.get(mode, mode)} 重试: {e}")
            time.sleep(0.3)
    return False


def _wait_wl(client: BridgeClient, want: int, timeout: float = 6.0) -> tuple[bool, dict]:
    """轮询 CMD_GET_WIRELESS_STATE 直到达到 want（或超时）。"""
    deadline = time.monotonic() + timeout
    last = {"state": None, "name": "?"}
    while time.monotonic() < deadline:
        try:
            last = client.get_wireless_state()
            if last["state"] == want:
                return True, last
        except Exception as e:  # noqa: BLE001
            last = {"state": None, "name": f"err:{e}"}
        time.sleep(0.3)
    return False, last


def run_single(via: str, port: str, baud: int) -> int:
    prof = VIA_PROFILES[via]
    print(f"通路：{prof['label']}")
    link = BridgeLink(SerialTransport(port, baud), baud=baud,
                      log=lambda s: print("  [link]", s))
    link.start()
    print(f"UART {port}@{baud} 已打开（扮演 SK32），等待 0.5s 稳定…")
    time.sleep(0.5)
    try:
        r = hid_roundtrip(prof, link)
    finally:
        link.stop()
    print("\n==== 结果 ====")
    print_result(prof, r)
    return 0 if (r["down"] and r["up"]) else 1


def run_both(port: str, baud: int) -> int:
    """单进程内完成双通路：唤醒 → MANUAL → BLE → 2.4G → 还原 BLE，最后打汇总表。"""
    print("== 无线 VIA 双通路测试（both）==")
    link = BridgeLink(SerialTransport(port, baud), baud=baud,
                      log=lambda s: print("  [link]", s))
    link.start()
    client = BridgeClient(link)
    print(f"UART {port}@{baud} 已打开（扮演 SK32），等待 0.3s 稳定…")
    time.sleep(0.3)

    if not _wake(client):
        print("!! 唤醒失败（SLEEP_WAKE 无 ACK），请检查串口/链路。")
        link.stop()
        return 2
    print("WAKE ACK")
    try:
        client.set_sleep_policy(proto.SLEEP_POLICY_MANUAL)
        print("sleep_policy -> MANUAL ACK")
    except Exception as e:  # noqa: BLE001
        print(f"  sleep_policy MANUAL 设置失败（继续）：{e}")

    results: dict = {}
    precond_ok = True

    # --- 通路 B：BLE ---
    print("\n---- 通路 B：BLE ----")
    _set_mode(client, proto.MODE_BLE)
    ok, st = _wait_wl(client, proto.WL_BLE_LINKED)
    print(f"  无线状态: {st}")
    if ok:
        results["ble"] = hid_roundtrip(VIA_PROFILES["ble"], link)
    else:
        precond_ok = False
        print("  !! BLE 未连接：需先在系统蓝牙中配对该键盘（见 SKILL §12.8）。")
        results["ble"] = {"down": False, "up": False, "evt_n": 0, "hid_in": None}

    # --- 通路 A：2.4G ---
    print("\n---- 通路 A：2.4G ----")
    _set_mode(client, proto.MODE_2G4)
    ok, st = _wait_wl(client, proto.WL_2G4_LINKED)
    if not ok:
        print(f"  未连接（{st}），尝试 pair_start …")
        try:
            client.pair_start()
        except Exception as e:  # noqa: BLE001
            print(f"  pair_start err: {e}")
        ok, st = _wait_wl(client, proto.WL_2G4_LINKED, timeout=8.0)
    print(f"  无线状态: {st}")
    if ok:
        results["2g4"] = hid_roundtrip(VIA_PROFILES["2g4"], link)
    else:
        precond_ok = False
        print("  !! 2.4G 未连接：需 Dongle 已配对（可先 pair_start）。")
        results["2g4"] = {"down": False, "up": False, "evt_n": 0, "hid_in": None}

    # --- 还原 BLE ---
    print("\n---- 还原 BLE ----")
    if _set_mode(client, proto.MODE_BLE):
        print("  已切回 BLE")

    link.stop()

    # --- 汇总 ---
    print("\n==== 汇总（下行=主机->键盘 / 上行=键盘->主机）====")
    all_pass = True
    for key in ("ble", "2g4"):
        r = results[key]
        ok = r["down"] and r["up"]
        all_pass = all_pass and ok
        print(f"  {VIA_PROFILES[key]['label']:<20} "
              f"下行 {'PASS' if r['down'] else 'FAIL'}  "
              f"上行 {'PASS' if r['up'] else 'FAIL'}  "
              f"→ {'PASS' if ok else 'FAIL'}")
    if all_pass:
        print("结论：双通路 PASS")
        return 0
    print("结论：存在 FAIL（若为前置未连接，见上方提示）")
    return 2 if not precond_ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--via", choices=["ble", "2g4", "both"], default="both")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()

    if args.via == "both":
        return run_both(args.port, args.baud)
    return run_single(args.via, args.port, args.baud)


if __name__ == "__main__":
    sys.exit(main())
