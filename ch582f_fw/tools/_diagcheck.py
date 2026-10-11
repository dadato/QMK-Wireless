"""临时工具：读取 2.4G 诊断快照 / 打印状态 / 触发配对。
用法:
    python _diagcheck.py read                 # 读 COM11(Dongle)+COM10(键盘) 状态与 diag
    python _diagcheck.py pair                 # Dongle PAIR_START + 键盘 SET_MODE3+PAIR_START
    python _diagcheck.py unpair               # 两端 UNPAIR(0xFF)
    python _diagcheck.py reset                # 两端 CMD_RESET 软复位（清零 diag 计数器）
    python _diagcheck.py verify               # 唤醒+配对+6s VIA 下行流量+两端 diag
    python _diagcheck.py soak [N]             # 大样本 VIA 2.4G 往返丢包率（默认 N=10000）
    python _diagcheck.py uartleg [N] [wait_ms]  # UART 腿隔离（只走 COM13，不经射频，默认 N=20000/wait=50）
"""
from __future__ import annotations

import sys
import time
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from sk32sim import proto  # noqa: E402

import serial  # noqa: E402

DONGLE = "COM18"   # USB\VID_1A86&PID_8010&MI_01\7&37DEB743&4&0001 (Dongle 侧 WCH-Link SERIAL)
KEYBD = "COM19"    # USB\VID_1A86&PID_8010&MI_01\8&20449A78&3&0001 (键盘侧 WCH-Link SERIAL)
BAUD = 921600

# 测试发送语义：一发一收，收到模组应答才算「发送完成」；无应答则整帧重发，
# 最多重发 RETRY_MAX 次（共 1 + RETRY_MAX 次尝试）。见 xact / do_uartleg / do_soak。
RETRY_MAX = 3

# 顺序与固件 sdk_rf24_diag() 的 c[0..7] 严格一致（见 bridge_rf24_ch58x.c:714-747）：
#   c[0]=tx_submit_ok c[1]=tx_submit_fail c[2]=tx_done_ok c[3]=tx_done_fail
#   c[4]=tx_drop（队列满 / 提交失败 / 重发耗尽丢帧） c[5]=rx_any c[6]=rx_pair c[7]=rx_data_bad
# 旧命名 "tx_timeout"/"hop_shut" 与固件语义错位，已更正。
DIAG_NAMES = [
    "tx_submit_ok", "tx_submit_fail", "tx_done_ok", "tx_done_fail",
    "tx_drop", "rx_any", "rx_pair", "rx_data_bad",
]

# UART 接收腿取证（键盘侧 CMD_GET_DIAG 尾部追加段，见 bridge_rf24_ch58x.c:sdk_rf24_diag）
#   [24..25] u_over  RX FIFO 溢出(overrun)   [26..27] u_frm RX 帧错
#   [28..29] u_par   RX 校验错               [30..31] u_half 半帧超时丢
#   [32..33] u_ring  ring 满丢               [34..35] u_len  长度越界丢
#   [36..37] u_crc   CRC 错                  [38..41] u_ok   合法帧数(LE32)
# 旧 24B 固件无此段 → 各字段读作 0（has_uart=False）。
UART_DIAG_NAMES = ["u_over", "u_frm", "u_par", "u_half", "u_ring", "u_len", "u_crc"]


def open_port(p: str) -> serial.Serial:
    return serial.Serial(p, BAUD, bytesize=8, parity="N", stopbits=1,
                         timeout=0, write_timeout=2.0)


def xact(ser: serial.Serial, cmd: int, payload: bytes = b"", wait: float = 0.7,
         retries: int = RETRY_MAX):
    """发送一帧并等模组应答，收到才算「发送完成」。

    语义（用户要求）：发出后 wait 秒内未收到 cmd 相同的应答帧，则**整帧重发**，
    最多重发 retries 次（共 1+retries 次尝试）。任一次收到匹配应答即返回该帧；
    全部耗尽仍无应答返回 None。"""
    seq = 0x40
    frame = proto.encode(cmd, seq, 0, payload)
    for attempt in range(retries + 1):
        parser = proto.FrameParser()
        try:
            ser.reset_input_buffer()
        except Exception:
            pass
        ser.write(frame)
        t0 = time.monotonic()
        frames = []
        while time.monotonic() - t0 < wait:
            n = ser.in_waiting
            if n:
                frames += parser.feed(ser.read(n))
            else:
                time.sleep(0.004)
        for fr in frames:
            if fr.cmd == cmd:
                return fr
        if attempt < retries:
            time.sleep(0.01)   # 重发前稍作让路
    return None


def _dongle_diag2_raw():
    """Dongle GET_DIAG2(0x07) → 上行丢帧取证计数（LE32）。

    回包：body[0]=status, [1..4]=ep3_pushed, [5..8]=ep3_q_drop,
          [9]=ep3_q_hwm, [10..13]=rf24_rx_overwrite。
    ep3_q_hwm≥2 即证明「上一包未被主机取走又来一帧」——旧实现（EP3 忙即丢）会在此丢帧。"""
    import hid  # noqa: E402

    for d in hid.enumerate():
        p = d["path"].decode(errors="replace")
        if d.get("vendor_id") == 0x0194 and d.get("product_id") == 0x0463 \
                and d.get("usage_page") == 0xFF60 and "mi_02" in p.lower():
            h = hid.device()
            h.open_path(d["path"])
            try:
                while h.read(64, 20):
                    pass
                h.write(b"\x00" + b"OTA" + bytes([0x07]) + bytes(28))
                for _ in range(20):
                    data = h.read(64, 200)
                    if data:
                        raw = bytes(data)
                        body = raw[:32] if len(raw) >= 32 else raw
                        if len(body) < 14:
                            return None
                        le32 = lambda o: (body[o] | (body[o + 1] << 8)
                                          | (body[o + 2] << 16) | (body[o + 3] << 24))
                        return {
                            "status": body[0],
                            "ep3_pushed": le32(1),
                            "ep3_q_drop": le32(5),
                            "ep3_q_hwm": body[9],
                            "rx_overwrite": le32(10),
                        }
            finally:
                h.close()
    return None


def read_diag(ser: serial.Serial):
    fr = xact(ser, proto.CMD_GET_DIAG, b"", 0.9)
    if not fr or len(fr.payload) < 24:
        return None
    p = fr.payload
    d = {
        "up": p[0], "conn": p[1], "bound": p[2],
        "tx_busy": p[3], "tx_q": p[4], "hb_miss": p[5],
        "period": p[6] | (p[7] << 8),
        # 跳频构建：out[7]=是否已锁频（非跳频构建下它是 period 高字节）
        "hop": p[7],
    }
    for i, nm in enumerate(DIAG_NAMES):
        d[nm] = p[8 + i * 2] | (p[9 + i * 2] << 8)
    return d


def dump(port: str, label: str) -> None:
    print(f"===== {label} ({port}) =====")
    try:
        ser = open_port(port)
    except Exception as e:  # noqa: BLE001
        print(f"  打开失败: {e}")
        return
    st = xact(ser, proto.CMD_GET_STATUS)
    print(f"  GET_STATUS : {st.payload.hex(' ') if st else '(无应答)'}")
    wl = xact(ser, proto.CMD_GET_WIRELESS_STATE)
    if wl and wl.payload:
        print(f"  WIRELESS   : {wl.payload.hex(' ')} -> {proto.wl_name(wl.payload[0])}")
    else:
        print("  WIRELESS   : (无应答)")
    md = xact(ser, proto.CMD_GET_MODE)
    if md and md.payload:
        p = md.payload
        print(f"  GET_MODE   : mode={proto.MODE_NAMES.get(p[0], p[0])} "
              f"conn={p[1] if len(p) > 1 else '?'} "
              f"radio={proto.radio_name(p[2]) if len(p) > 2 else '?'}")
    d = read_diag(ser)
    if d:
        print("  DIAG(24B)  :")
        print(f"    up={d['up']} conn={d['conn']} bound={d['bound']} "
              f"tx_busy={d['tx_busy']} tx_q={d['tx_q']} hb_miss={d['hb_miss']} "
              f"hop={d['hop']} period={d['period']}ms")
        for nm in DIAG_NAMES:
            print(f"    {nm:16s}= {d[nm]}")
        print(f"    >>> rx_any={d['rx_any']} rx_pair={d['rx_pair']} "
              f"rx_data_bad={d['rx_data_bad']}  (bad/any="
              f"{(d['rx_data_bad'] / d['rx_any'] * 100 if d['rx_any'] else 0):.1f}%)")
    else:
        print("  DIAG       : (无应答)")
    ser.close()


def do_pair() -> None:
    # Dongle 先进入 2.4G 配对窗口
    print("[pair] Dongle PAIR_START ...")
    ser = open_port(DONGLE)
    print("   ", xact(ser, proto.CMD_PAIR_START, b"", 0.7))
    ser.close()
    time.sleep(0.2)
    # 键盘切 2.4G 并进入配对
    print("[pair] 键盘 SET_MODE 2G4 + PAIR_START ...")
    ser = open_port(KEYBD)
    print("    set_mode:", xact(ser, proto.CMD_SET_MODE, bytes((proto.MODE_2G4, 1)), 0.7))
    time.sleep(0.3)
    print("    pair    :", xact(ser, proto.CMD_PAIR_START, b"", 0.7))
    ser.close()
    # 等待配对完成
    print("[pair] 等待 3s ...")
    time.sleep(3.0)


def do_raw(port: str, baud: int = BAUD, listen_s: float = 5.0) -> None:
    """被动监听 listen_s 秒（不写），打印全部原始字节。"""
    ser = serial.Serial(port, baud, bytesize=8, parity="N", stopbits=1,
                        timeout=0, write_timeout=2.0)
    print(f"[raw] {port} @ {baud} 被动监听 {listen_s}s ...")
    t0 = time.monotonic()
    got = 0
    while time.monotonic() - t0 < listen_s:
        n = ser.in_waiting
        if n:
            d = ser.read(n)
            got += len(d)
            print("   RAW", d.hex(" "))
        else:
            time.sleep(0.01)
    print(f"[raw] 共收到 {got} 字节。")
    ser.close()


def dongle_diag() -> None:
    """Dongle 不走 UART1；diag 经 USB vendor HID(MI_02, Usage 0xFF60, RID0, 32B)：
    写 'OTA'+GET_INFO(0x06) -> IN 回 [status][mode][fw u16][vid u16][pid u16][diag 24B]。"""
    import hid  # noqa: E402

    dev = None
    for d in hid.enumerate():
        p = d["path"].decode(errors="replace")
        if d.get("vendor_id") == 0x0194 and d.get("product_id") == 0x0463 \
                and d.get("usage_page") == 0xFF60 and "mi_02" in p.lower():
            dev = d
            break
    if dev is None:
        print("  未找到 Dongle vendor HID (MI_02 / Usage 0xFF60)")
        return
    print(f"  HID: {dev['path'].decode(errors='replace')}")
    h = hid.device()
    h.open_path(dev["path"])
    try:
        while h.read(64, 20):
            pass
        req = b"\x00" + b"OTA" + bytes([0x06]) + bytes(28)   # RID0 前缀 + 'OTA'+GET_INFO
        n = h.write(req)
        print(f"  write {n}B -> {req[:8].hex(' ')} ...")
        raw = None
        for _ in range(20):
            data = h.read(64, 200)
            if data:
                raw = bytes(data)
                break
        if raw is None:
            print("  (无 IN 应答)")
            return
        print(f"  IN {len(raw)}B: {raw.hex(' ')}")
        # RID0 无前缀(32B) 或带 1B 前缀(33B)
        body = raw[:32] if (len(raw) == 32 or (raw[0] != 0x00 and len(raw) >= 32)) else raw[1:33]
        status = body[0]
        # 布局：body[0]=status, [1]=mode, [2..3]=fw, [4..5]=vid, [6..7]=pid, [8..31]=diag 24B
        p = body[8:8 + 24]
        print(f"  status={status:#04x} mode={body[1]:#04x} "
              f"fw={body[2] | (body[3] << 8):#06x} "
              f"vid={body[4] | (body[5] << 8):#06x} pid={body[6] | (body[7] << 8):#06x}")
        print(f"  原始 diag 24B: {p.hex(' ')}")
        d = {
            "up": p[0], "conn": p[1], "bound": p[2],
            "tx_busy": p[3], "tx_q": p[4], "hb_miss": p[5],
            "period": p[6] | (p[7] << 8),
            "hop": p[7],
        }
        for i, nm in enumerate(DIAG_NAMES):
            d[nm] = p[8 + i * 2] | (p[9 + i * 2] << 8)
        print(f"    up={d['up']} conn={d['conn']} bound={d['bound']} "
              f"tx_busy={d['tx_busy']} tx_q={d['tx_q']} hb_miss={d['hb_miss']} "
              f"hop={d['hop']} period={d['period']}ms")
        for nm in DIAG_NAMES:
            print(f"    {nm:16s}= {d[nm]}")
        print(f"    >>> rx_any={d['rx_any']} rx_pair={d['rx_pair']} "
              f"rx_data_bad={d['rx_data_bad']}  (bad/any="
              f"{(d['rx_data_bad'] / d['rx_any'] * 100 if d['rx_any'] else 0):.1f}%)")
    finally:
        h.close()


def _parse_diag24(p: bytes) -> dict:
    d = {
        "up": p[0], "conn": p[1], "bound": p[2],
        "tx_busy": p[3], "tx_q": p[4], "hb_miss": p[5],
        "period": p[6] | (p[7] << 8),
        "hop": p[7],
    }
    for i, nm in enumerate(DIAG_NAMES):
        d[nm] = p[8 + i * 2] | (p[9 + i * 2] << 8)
    # UART 接收腿取证段（[24..41]，仅 42B 新固件有；缺失则恒 0）
    for i, nm in enumerate(UART_DIAG_NAMES):
        off = 24 + i * 2
        d[nm] = (p[off] | (p[off + 1] << 8)) if len(p) >= off + 2 else 0
    d["u_ok"] = (p[38] | (p[39] << 8) | (p[40] << 16) | (p[41] << 24)) \
        if len(p) >= 42 else 0
    d["has_uart"] = len(p) >= 42
    return d


def _show(label: str, d: dict) -> None:
    print(f"  [{label}] up={d['up']} conn={d['conn']} bound={d['bound']} "
          f"hop={d.get('hop', 0)} "
          f"sub_ok={d['tx_submit_ok']} sub_fail={d['tx_submit_fail']} "
          f"done_ok={d['tx_done_ok']} done_fail={d['tx_done_fail']} "
          f"drop={d['tx_drop']} rx_pair={d['rx_pair']}")
    print(f"  [{label}] >>> rx_any={d['rx_any']} "
          f"rx_data_bad={d['rx_data_bad']}  (bad/any="
          f"{(d['rx_data_bad'] / d['rx_any'] * 100 if d['rx_any'] else 0):.1f}%)")
    if d.get("has_uart"):
        print(f"  [{label}] UART腿: over={d['u_over']} frm={d['u_frm']} "
              f"par={d['u_par']} half={d['u_half']} ring={d['u_ring']} "
              f"len={d['u_len']} crc={d['u_crc']} ok={d['u_ok']}")


def _print_delta(label: str, a: dict, b: dict) -> None:
    """打印全字段增量（按 16 位回绕取模）。

    ★勿用 done_ok 当「纯 TX 尝试」推 ACK 成功率：固件里 tx_done_ok 同时计入
    TX_MODE_TX_FINISH（本端自发 TX「已发出」，非终态）与 RX_MODE_TX_FINISH
    （收到对端数据后自动回 ACK 成功），二者混在一起，据此算的百分比会被高估。
    正确的关系式见下行注释。"""
    parts = [f"{nm}={((b[nm] - a[nm]) & 0xFFFF)}" for nm in DIAG_NAMES]
    print(f"  [{label}] 增量: " + "  ".join(parts))
    # UART 接收腿增量（仅两端都有 42B 段时）：判定「上行丢包是否卡在键盘 UART RX」
    if a.get("has_uart") and b.get("has_uart"):
        uparts = [f"{nm}={((b[nm] - a[nm]) & 0xFFFF)}" for nm in UART_DIAG_NAMES]
        uparts.append(f"u_ok=+{(b['u_ok'] - a['u_ok']) & 0xFFFFFFFF}")
        print(f"  [{label}] UART腿增量: " + "  ".join(uparts))
    # 语义（见 bridge_rf24_ch58x.c:323-415）：
    #   tx_submit_ok  = RF_Tx 提交成功次数（≈ TX_MODE_TX_FINISH 次数，本端自发 TX 尝试）
    #   tx_submit_fail= RF_Tx 提交失败（抢占/忙）
    #   tx_done_ok    = TX_MODE_TX_FINISH + RX_MODE_TX_FINISH（自发发出 + 自动回 ACK 成功）
    #   tx_done_fail  = TX_MODE_RX_TIMEOUT + TX_MODE_TX_FAIL + RX_MODE_TX_FAIL（自发未等到 ACK + 回 ACK 失败）
    #   tx_drop       = 队列满丢最旧 + 重发耗尽丢帧（射频层真实丢帧）
    # 故：自动回 ACK 成功数 ≈ (tx_done_ok - tx_submit_ok)，应≈ rx_any（每收到一帧回一 ACK）。


def _dongle_diag_raw():
    """返回 Dongle 的 24B diag（USB vendor HID MI_02, 'OTA'+GET_INFO）。"""
    import hid  # noqa: E402

    for d in hid.enumerate():
        p = d["path"].decode(errors="replace")
        if d.get("vendor_id") == 0x0194 and d.get("product_id") == 0x0463 \
                and d.get("usage_page") == 0xFF60 and "mi_02" in p.lower():
            h = hid.device()
            h.open_path(d["path"])
            try:
                while h.read(64, 20):
                    pass
                h.write(b"\x00" + b"OTA" + bytes([0x06]) + bytes(28))
                for _ in range(20):
                    data = h.read(64, 200)
                    if data:
                        raw = bytes(data)
                        body = raw[:32] if len(raw) >= 32 else raw
                        return body[8:8 + 24]
            finally:
                h.close()
    return None


def do_verify(traffic_ms: int = 6000) -> None:
    """单进程：键盘唤醒→MANUAL→2G4 配对→持续 VIA 32B 往返产生射频流量→两端读 diag。"""
    from sk32sim.link import BridgeLink, SerialTransport  # noqa: E402
    from sk32sim.client import BridgeClient  # noqa: E402
    from sk32sim import proto as P  # noqa: E402
    import hid  # noqa: E402

    TX = bytes([0x01, 0x00, 0x0D]) + bytes(29)

    link = BridgeLink(SerialTransport(KEYBD, BAUD), baud=BAUD,
                      log=lambda s: print("  [link]", s))
    link.start()
    client = BridgeClient(link)
    time.sleep(0.3)

    print("[verify] 唤醒 + MANUAL ...")
    for _ in range(8):
        try:
            client.sleep(P.SLEEP_WAKE)
            break
        except Exception:
            time.sleep(0.3)
    try:
        client.set_sleep_policy(1)
    except Exception as e:  # noqa: BLE001
        print("   sleep_policy:", e)

    print("[verify] set_mode 2G4 + 等待 2G4_LINKED ...")
    try:
        client.set_mode(P.MODE_2G4)
    except Exception as e:  # noqa: BLE001
        print("   set_mode:", e)

    linked = False
    for attempt in range(3):
        for _ in range(12):
            try:
                st = client.get_wireless_state()
            except Exception as e:  # noqa: BLE001
                st = {"state": None, "name": f"err:{e}"}
            if st["state"] == P.WL_2G4_LINKED:
                linked = True
                break
            time.sleep(0.3)
        if linked:
            break
        print(f"   第{attempt + 1}次 pair_start ...")
        try:
            client.pair_start()
        except Exception as e:  # noqa: BLE001
            print("   pair_start:", e)
        time.sleep(1.0)
    print(f"[verify] 2G4 链路: {'LINKED' if linked else '未连接 '+str(st)}")

    # 打开 Dongle vendor HID 以产生下行流量
    dev = None
    for d in hid.enumerate():
        p = d["path"].decode(errors="replace")
        if d.get("vendor_id") == 0x0194 and d.get("product_id") == 0x0463 \
                and d.get("usage_page") == 0xFF60 and "mi_02" in p.lower():
            dev = d
            break
    evt_n = 0

    def on_event(fr):
        nonlocal evt_n
        if fr.cmd == P.EVT_TUNNEL_USB:
            evt_n += 1
            link.send_hid(P.CMD_TUNNEL_USB, TX)

    link.on_event = on_event

    if dev is None:
        print("   !! 未找到 Dongle vendor HID，跳过流量生成")
    else:
        h = hid.device()
        h.open_path(dev["path"])
        try:
            while h.read(64, 20):
                pass
            print(f"[verify] 持续 {traffic_ms / 1000:.0f}s 发 32B VIA 下行帧 ...")
            t0 = time.monotonic()
            n_tx = 0
            while (time.monotonic() - t0) * 1000 < traffic_ms:
                h.write(b"\x00" + TX)
                n_tx += 1
                h.read(64, 50)
                time.sleep(0.05)
            print(f"   下行写 {n_tx} 帧；键盘侧 EVT_TUNNEL_USB 收到 {evt_n} 帧")
        finally:
            h.close()

    time.sleep(0.5)

    print("\n==== 修复后 2.4G diag ====")
    try:
        fr = link.request(P.CMD_GET_DIAG, timeout=1.0)
        if fr and len(fr.payload) >= 24:
            _show("键盘", _parse_diag24(fr.payload))
    except Exception as e:  # noqa: BLE001
        print("  键盘 GET_DIAG 失败:", e)
    dd = _dongle_diag_raw()
    if dd:
        _show("Dongle", _parse_diag24(dd))
    else:
        print("  Dongle diag 读取失败")

    link.stop()


def do_soak(count: int = 10000, wait_ms: int = 50) -> None:
    """大样本 VIA 2.4G 往返丢包率（严格一发一收）。

    流程：写 Dongle vendor HID(RID0,32B，payload[28..31]=LE32 序号)
          -> 2.4G 下行 -> 键盘模组 UART EVT_TUNNEL_USB（本脚本按原 payload 回显）
          -> 2.4G 上行 -> Dongle vendor IN；逐帧按序号配对统计成功/丢包。
    时序（测试发送语义）：发一帧 -> 只等**本帧序号**的应答（校验响应与顺序），
          收到才记「发送完成」并进下一帧；超过 wait_ms 无应答则**整帧重发**
          （同序号幂等回显），最多重发 RETRY_MAX 次；重发耗尽仍无应答才记丢包。
    """
    from sk32sim.link import BridgeLink, SerialTransport  # noqa: E402
    from sk32sim.client import BridgeClient  # noqa: E402
    from sk32sim import proto as P  # noqa: E402
    import hid  # noqa: E402

    OFF = 28  # 计数值置于 payload[28..31]（LE32），VIA 语义字段 [0..2] 不动
    #  ★用 32 位而非 16 位：100000 帧下 16 位序号会回绕 ~1.5 次，「按方向拆分」时
    #    会把 i>=65536 的帧与更早的同序号帧混为一谈（i not in dl_seqs 恒真），
    #    历史 100000 跑出的「下行丢 34464 / 34.46%」即该假象。32 位在 4e9 帧内
    #    不回绕，逐帧唯一 → 方向拆分与根因判别才可信。

    def mk_frame(i: int) -> bytes:
        p = bytearray(32)
        p[0], p[1], p[2] = 0x01, 0x00, 0x0D
        p[OFF] = i & 0xFF
        p[OFF + 1] = (i >> 8) & 0xFF
        p[OFF + 2] = (i >> 16) & 0xFF
        p[OFF + 3] = (i >> 24) & 0xFF
        return bytes(p)

    def seq_of(body: bytes):
        if len(body) < OFF + 4:
            return None
        return (body[OFF] | (body[OFF + 1] << 8)
                | (body[OFF + 2] << 16) | (body[OFF + 3] << 24))

    link = BridgeLink(SerialTransport(KEYBD, BAUD, idle_poll_s=0.001), baud=BAUD,
                      gap_ms=1.0, log=lambda s: None)
    link.start()
    client = BridgeClient(link)
    time.sleep(0.3)

    print("[soak] 唤醒 + MANUAL + 关心跳 + 2G4 配对 ...")
    for _ in range(8):
        try:
            client.sleep(P.SLEEP_WAKE)
            break
        except Exception:  # noqa: BLE001
            time.sleep(0.3)
    try:
        client.set_sleep_policy(1)          # MANUAL：关自动休眠
    except Exception:  # noqa: BLE001
        pass
    try:
        client.set_heartbeat(False)         # 关桥侧心跳（不发 PING、不做失联判定）
    except Exception:  # noqa: BLE001
        pass
    try:
        client.set_mode(P.MODE_2G4)
    except Exception:  # noqa: BLE001
        pass

    linked = False
    for attempt in range(3):
        for _ in range(12):
            try:
                st = client.get_wireless_state()
            except Exception as e:  # noqa: BLE001
                st = {"state": None, "name": f"err:{e}"}
            if st["state"] == P.WL_2G4_LINKED:
                linked = True
                break
            time.sleep(0.3)
        if linked:
            break
        print(f"   第{attempt + 1}次 pair_start ...")
        try:
            client.pair_start()
        except Exception as e:  # noqa: BLE001
            print("   pair_start:", e)
        time.sleep(1.0)
    print(f"[soak] 2G4 链路: {'LINKED' if linked else '未连接 '+str(st)}")

    dl_seqs: set = set()   # 下行到达模组的帧序号（接收器→模组 方向）
    ul_seqs: set = set()   # 上行到达接收器的帧序号（模组→接收器 方向）
    dl_events = 0          # 模组吐出的下行事件数（含重传导致的重复）
    ul_t0: dict = {}       # 序号 -> 模组回显实际写入 UART 的时刻（上行时延基准）
    ul_lat: list = []      # 上行单程时延 ms（回显写入模组 → 上位机经 Dongle 收到）
    echo_ok = 0            # 回显成功写入 UART 的帧数（上行 PC 侧已确认发出）
    echo_fail = 0          # 回显写失败帧数（send_hid 抛异常被吞 → 静默丢点）
    echo_fail_seqs: set = set()   # 回显写失败对应帧序号（供上行丢帧归因）

    def _ours(body: bytes) -> bool:
        # 本工具注入帧的固定特征：payload[0..2] = 01 00 0D（端到端逐字节透传）
        return len(body) >= OFF + 4 and body[0] == 0x01 and body[1] == 0x00 \
            and body[2] == 0x0D

    def on_event(fr):
        nonlocal dl_events, echo_ok, echo_fail
        if fr.cmd == P.EVT_TUNNEL_USB:
            body = fr.payload[:32]
            s = seq_of(body)
            mine = s is not None and _ours(body)
            if mine:
                dl_seqs.add(s)
                dl_events += 1
            try:
                link.send_hid(P.CMD_TUNNEL_USB, body)  # 原 payload 回显
                if mine:
                    echo_ok += 1
                    # 回显已实际写入 UART（send_hid 返回即写完成，已含 gap 等待）→ 打时延基准
                    if len(ul_t0) > 16:
                        ul_t0.clear()
                    ul_t0[s] = time.monotonic()
            except Exception:  # noqa: BLE001
                if mine:
                    echo_fail += 1          # ★静默丢点取证：不再无声吞掉写失败
                    echo_fail_seqs.add(s)

    link.on_event = on_event

    kb0 = None
    try:
        fr = link.request(P.CMD_GET_DIAG, timeout=1.0)
        if fr and len(fr.payload) >= 24:
            kb0 = _parse_diag24(fr.payload)
    except Exception:  # noqa: BLE001
        pass
    dd0 = _dongle_diag_raw()
    dd2_0 = _dongle_diag2_raw()   # 32 位不饱和：EP3 上行队列 / 收包覆写 取证基准

    dev = None
    for d in hid.enumerate():
        p = d["path"].decode(errors="replace")
        if d.get("vendor_id") == 0x0194 and d.get("product_id") == 0x0463 \
                and d.get("usage_page") == 0xFF60 and "mi_02" in p.lower():
            dev = d
            break
    if dev is None:
        print("!! 未找到 Dongle vendor HID，退出")
        link.stop()
        return

    h = hid.device()
    h.open_path(dev["path"])
    sent = delivered = write_fail = retry = 0
    lat: list = []
    lost_recs: list = []   # 每个「重发耗尽仍无应答」帧：(seq, 注入后经过秒数)
    mismatch = 0           # 应答序号 != 本帧序号（错序/重复/串帧）
    mismatch_recs: list = []
    t_all0 = time.monotonic()
    prev_delivered = prev_dl = prev_ul = 0
    prev_crc = prev_efail = prev_retry = 0   # PC 侧 CRC 错 / 回显写失败 / 重发次数 窗口基准
    last_snap_t = [0.0]            # 上次取快照时刻（限频，避免打扰 soak 时序）

    def _snap(tag: str) -> None:
        """突发取证：抓两端 24B diag 关键字段（走独立通道，不占 2.4G）。
        仅在某窗口丢包异常时调用，稳态不打扰 soak 时序。"""
        row = [f"    [{tag}]"]
        fmt = ("up=%d conn=%d bound=%d busy=%d q=%d miss=%d sub_ok=%d sub_fail=%d "
               "done_ok=%d done_fail=%d drop=%d rx_any=%d bad=%d")
        try:
            fr = link.request(P.CMD_GET_DIAG, timeout=1.0)
            if fr and len(fr.payload) >= 24:
                d = _parse_diag24(fr.payload)
                kb = fmt % (d["up"], d["conn"], d["bound"], d["tx_busy"],
                            d["tx_q"], d["hb_miss"], d["tx_submit_ok"], d["tx_submit_fail"],
                            d["tx_done_ok"], d["tx_done_fail"], d["tx_drop"],
                            d["rx_any"], d["rx_data_bad"])
                if d.get("has_uart"):
                    kb += (f" |UART over={d['u_over']} half={d['u_half']} "
                           f"ring={d['u_ring']} crc={d['u_crc']} ok={d['u_ok']}")
                row.append("KB(" + kb + ")")
            else:
                row.append("KB(读失败)")
        except Exception as e:  # noqa: BLE001
            row.append(f"KB(err:{e})")
        dd = _dongle_diag_raw()
        if dd:
            d = _parse_diag24(dd)
            row.append("DG(" + fmt % (d["up"], d["conn"], d["bound"], d["tx_busy"],
                       d["tx_q"], d["hb_miss"], d["tx_submit_ok"], d["tx_submit_fail"],
                       d["tx_done_ok"], d["tx_done_fail"], d["tx_drop"],
                       d["rx_any"], d["rx_data_bad"]) + ")")
        else:
            row.append("DG(读失败)")
        dd2 = _dongle_diag2_raw()
        if dd2:
            row.append("DG2(push=%d qdrop=%d hwm=%d ovw=%d)"
                       % (dd2["ep3_pushed"], dd2["ep3_q_drop"],
                          dd2["ep3_q_hwm"], dd2["rx_overwrite"]))
        print(" ".join(row), flush=True)

    def _rec_in(raw: bytes):
        """记录一次有效 IN 到达：返回序号（非本工具帧返回 None），并按需采上行时延。

        上行时延 = 上位机读到该回显的时刻 - 模组把回显写入 UART 的时刻（on_event 打点），
        即「模组→接收器→USB→上位机」的单程耗时，与往返（PC 发起到收到）解耦。"""
        t_read = time.monotonic()
        rb = raw[1:33] if (len(raw) == 33 and raw[0] == 0) else raw[:32]
        s = seq_of(rb)
        if s is None or not _ours(rb):
            return None
        ul_seqs.add(s)
        t1 = ul_t0.pop(s, None)
        if t1 is not None:
            ul_lat.append((t_read - t1) * 1000.0)
        return s

    try:
        while True:  # 清残留 IN（同时计入上行到达）
            d = h.read(64, 10)
            if not d:
                break
            _rec_in(bytes(d))
        for i in range(count):
            # 严格「一发一收」：不预置帧间隔。发本帧后只等**它自己的序号**；
            # 收到（序号相等）才记「发送完成」，否则整帧重发（≤RETRY_MAX 次）。
            expect = i  # 32 位计数值：逐帧唯一，可与应答序号严格比对
            ok = False
            submitted = False
            t0 = None
            for attempt in range(RETRY_MAX + 1):
                while True:  # 清残留：上一帧/上一次尝试迟到的应答，仅计入上行到达
                    d = h.read(64, 1)
                    if not d:
                        break
                    _rec_in(bytes(d))
                try:
                    wn = h.write(b"\x00" + mk_frame(i))
                except Exception:  # noqa: BLE001
                    wn = 0
                if wn < 33:  # HID 写失败/短写，不计入提交（避免误算为射频丢包）
                    if attempt < RETRY_MAX:
                        retry += 1
                    continue
                if not submitted:
                    submitted = True
                    sent += 1
                t0 = time.monotonic()
                deadline = t0 + wait_ms / 1000.0
                while time.monotonic() < deadline:
                    data = h.read(64, 20)
                    if not data:
                        continue
                    s = _rec_in(bytes(data))
                    if s is None:
                        continue
                    if s == expect:
                        ok = True
                        break
                    # 序号与本帧不符：迟到/重复/错序应答 —— 计入错序，本帧不算成功
                    mismatch += 1
                    mismatch_recs.append((i, s))
                if ok:
                    break
                if attempt < RETRY_MAX:
                    retry += 1   # 无应答 → 整帧重发（同序号，幂等回显）
            if not submitted:
                write_fail += 1   # 本帧所有尝试均写失败 → 未提交，不计入射频丢包
                continue
            if ok:
                delivered += 1
                lat.append((time.monotonic() - t0) * 1000.0)
            else:
                lost_recs.append((i, time.monotonic() - t_all0))
            if (i + 1) % 100 == 0:
                win_ok = delivered - prev_delivered
                win_lost = 100 - win_ok
                win_dl = dl_events - prev_dl
                win_ul = len(ul_seqs) - prev_ul
                win_crc = link.stats.crc_errors - prev_crc
                win_efail = echo_fail - prev_efail
                win_retry = retry - prev_retry
                prev_delivered, prev_dl, prev_ul = delivered, dl_events, len(ul_seqs)
                prev_crc, prev_efail, prev_retry = link.stats.crc_errors, echo_fail, retry
                extra = ""
                if win_lost or win_crc or win_efail or win_retry:
                    extra = (f"  [本窗 丢{win_lost} 重发{win_retry} 下行到达+{win_dl} "
                             f"上行到达+{win_ul} crc{win_crc} 回显写失败{win_efail}]")
                print(f"   ...{i + 1}/{count}  往返成功 {delivered}  丢 {sent - delivered}  "
                      f"写失败 {write_fail}{extra}", flush=True)
                # 任一窗口出现丢包即取证（限频 5s）：抓两端 24B diag，
                # 判别丢在射频层(tx_drop) 还是上层(Dongle EP3 / PC 回显)
                if win_lost >= 1 and (time.monotonic() - last_snap_t[0]) > 5.0:
                    last_snap_t[0] = time.monotonic()
                    print(f"    [burst] 窗口 {i - 99}..{i}: 本窗丢 {win_lost}/100  "
                          f"下行到达事件 +{win_dl}  上行到达 +{win_ul}", flush=True)
                    _snap(f"snap@{i}")
    finally:
        h.close()
    dt = time.monotonic() - t_all0

    lost = sent - delivered
    print(f"\n==== VIA 2.4G 往返丢包统计（{count} 次）====")
    print(f"  提交 {sent}  往返成功 {delivered}  丢包 {lost}  "
          f"丢包率 {(lost / sent * 100 if sent else 0):.2f}%  HID写失败 {write_fail}")
    print(f"  无应答整帧重发 {retry} 次（≤{RETRY_MAX} 次/帧；重发耗尽才记丢包）")
    print(f"  序号校验: 应答错序/重复 {mismatch}（其中超时无应答 {lost}）")
    print(f"  PC 侧串口: 收帧 {link.stats.rx_frames}  CRC 错 {link.stats.crc_errors}  "
          f"事件 {link.stats.events}  超时 {link.stats.timeouts}")
    print(f"  回显写 UART: 成功 {echo_ok}  失败 {echo_fail}"
          + (f"  (失败 seq 前20: {sorted(echo_fail_seqs)[:20]})" if echo_fail_seqs else ""))
    if mismatch_recs:
        print(f"  错序明细 (发帧seq→应答seq) 前50: {mismatch_recs[:50]}")
    if lat:
        lat.sort()
        print(f"  往返时延 ms: 平均 {sum(lat) / len(lat):.1f}  "
              f"中位 {lat[len(lat) // 2]:.1f}  P95 {lat[int(len(lat) * 0.95)]:.1f}  "
              f"最大 {lat[-1]:.1f}")
    if ul_lat:
        ul_lat.sort()
        print(f"  上行时延 ms: 平均 {sum(ul_lat) / len(ul_lat):.1f}  "
              f"中位 {ul_lat[len(ul_lat) // 2]:.1f}  P95 {ul_lat[int(len(ul_lat) * 0.95)]:.1f}  "
              f"最大 {ul_lat[-1]:.1f}  (样本 {len(ul_lat)})")
    print(f"  耗时 {dt:.1f}s  速率 {sent / dt:.1f} 帧/s")

    # ---- 按方向拆分（按去重后的逻辑帧序号统计）----
    dl_sent = sent                       # 接收器(Dongle)已接受注入
    dl_recv = len(dl_seqs)               # 模组实际收到（下行送达）
    ul_sent = dl_recv                    # 模组对每个到达帧回显一次
    ul_recv = len(ul_seqs)               # 接收器实际收到回显（上行送达）

    def _pct(lost: int, tot: int) -> str:
        return f"{(lost / tot * 100 if tot else 0):.2f}%"

    print("\n---- 方向拆分 ----")
    print(f"  接收器→模组 (下行): 注入 {dl_sent}  模组收到 {dl_recv}  "
          f"丢 {dl_sent - dl_recv}  {_pct(dl_sent - dl_recv, dl_sent)}")
    print(f"  模组→接收器 (上行): 回显 {ul_sent}  接收器收到 {ul_recv}  "
          f"丢 {ul_sent - ul_recv}  {_pct(ul_sent - ul_recv, ul_sent)}")
    print(f"  端到端往返        : 注入 {dl_sent}  往返成功 {delivered}  "
          f"丢 {dl_sent - delivered}  {_pct(dl_sent - delivered, dl_sent)}")
    print(f"  （下行到达事件含重复共 {dl_events} 次，去重后 {dl_recv}）")

    # ---- 逐丢帧定界：每个未往返成功的帧，判定卡在「下行」还是「上行」----
    dl_lost: list = []
    ul_lost: list = []
    late: list = []
    if lost_recs:
        dl_lost = [s for s, _t in lost_recs if s not in dl_seqs]
        ul_lost = [s for s, _t in lost_recs if s in dl_seqs and s not in ul_seqs]
        late = [s for s, _t in lost_recs if s in dl_seqs and s in ul_seqs]
        print("\n---- 丢帧明细（seq@秒）----")
        print(f"  下行丢（未到模组）      {len(dl_lost)}: {dl_lost}")
        print(f"  上行丢（到模组未回接收器）{len(ul_lost)}: {ul_lost}")
        if ul_lost:
            ef = [s for s in ul_lost if s in echo_fail_seqs]
            print(f"    ├ PC 回显写失败 {len(ef)}: {ef}")
            print(f"    └ 回显已写出但上行未回 {len(ul_lost) - len(ef)}"
                  f"（→ 键盘射频上行 / Dongle 收包后 USB EP3）")
        if late:
            print(f"  双达但本帧窗口内未配对(迟到){len(late)}: {late}")
        print("  时间线: " + "  ".join(f"{s}@{t:.1f}s" for s, t in lost_recs))

    # ---- 根因判别：射频层丢帧 vs 接收器 USB EP3 忙即丢 ----
    kb1 = None
    try:
        fr = link.request(P.CMD_GET_DIAG, timeout=1.0)
        if fr and len(fr.payload) >= 24:
            kb1 = _parse_diag24(fr.payload)
            _show("键盘(后)", kb1)
            if kb0:
                _print_delta("键盘", kb0, kb1)
    except Exception as e:  # noqa: BLE001
        print("  键盘 GET_DIAG(后) 失败:", e)
    dd1 = _dongle_diag_raw()
    dg1 = _parse_diag24(dd1) if dd1 else None
    dg0 = _parse_diag24(dd0) if dd0 else None
    if dg1:
        _show("Dongle(后)", dg1)
        if dg0:
            _print_delta("Dongle", dg0, dg1)

    # ---- Dongle 上行取证：EP3 软件 FIFO / 收包覆写（32 位不饱和计数）----
    dd2_1 = _dongle_diag2_raw()
    dd2 = None
    if dd2_0 and dd2_1:
        dd2 = {
            "pushed": dd2_1["ep3_pushed"] - dd2_0["ep3_pushed"],
            "q_drop": dd2_1["ep3_q_drop"] - dd2_0["ep3_q_drop"],
            "hwm": dd2_1["ep3_q_hwm"],
            "overwrite": dd2_1["rx_overwrite"] - dd2_0["rx_overwrite"],
        }
        print("\n---- Dongle 上行取证 (GET_DIAG2) ----")
        print(f"  EP3 入队 +{dd2['pushed']}  队满丢 {dd2['q_drop']}  "
              f"队列高水位 {dd2['hwm']}  收包覆写丢 {dd2['overwrite']}")
        if dd2["hwm"] >= 2 and dd2["q_drop"] == 0:
            print("  → 高水位≥2 且队满丢 0：曾出现「上一包未被主机取走又来一帧」"
                  "（旧实现 EP3 忙即丢在此必丢）→ 软件 FIFO 已挽救，根因确认")
    else:
        print("\n---- Dongle 上行取证 (GET_DIAG2) ----\n  读取失败（固件未含 0x07 ?）")

    if lost_recs and kb0 and kb1 and dg0 and dg1:
        kb_drop = (kb1["tx_drop"] - kb0["tx_drop"]) & 0xFFFF   # 键盘射频层丢帧（上行 TX 源）
        dg_drop = (dg1["tx_drop"] - dg0["tx_drop"]) & 0xFFFF   # 接收器射频层丢帧（下行 TX 源）
        print("\n---- 根因判别 ----")
        print(f"  键盘(上行TX源)   射频层丢帧 tx_drop 增量 = {kb_drop}")
        print(f"  接收器(下行TX源) 射频层丢帧 tx_drop 增量 = {dg_drop}")
        if ul_lost or kb_drop:
            verdict = ("射频层重发耗尽（键盘 TX 丢帧）" if kb_drop >= len(ul_lost)
                       else "接收器收包后 USB EP3 忙即丢（射频已送达）")
            print(f"  上行: 丢帧 {len(ul_lost)}  vs 键盘 tx_drop 增量 {kb_drop} → {verdict}")
        if dl_lost or dg_drop:
            verdict = ("射频层重发耗尽（接收器 TX 丢帧）" if dg_drop >= len(dl_lost)
                       else "模组收包后 UART/队列丢弃（射频已送达）")
            print(f"  下行: 丢帧 {len(dl_lost)}  vs 接收器 tx_drop 增量 {dg_drop} → {verdict}")

    # ---- UART 接收腿取证：判定上行丢包是否卡在「PC→键盘 UART 写 / 键盘 UART RX」----
    if kb0 and kb1 and kb0.get("has_uart") and kb1.get("has_uart"):
        u = {nm: (kb1[nm] - kb0[nm]) & 0xFFFF for nm in UART_DIAG_NAMES}
        u_ok = (kb1["u_ok"] - kb0["u_ok"]) & 0xFFFFFFFF
        print("\n---- 键盘 UART 接收腿取证 ----")
        print(f"  RX 溢出(overrun) +{u['u_over']}  帧错 +{u['u_frm']}  校验错 +{u['u_par']}  "
              f"半帧超时 +{u['u_half']}  ring满 +{u['u_ring']}  长度越界 +{u['u_len']}  "
              f"CRC错 +{u['u_crc']}  合法帧 +{u_ok}")
        u_rx_err = u["u_over"] + u["u_frm"] + u["u_par"] + u["u_half"] \
            + u["u_ring"] + u["u_len"] + u["u_crc"]
        print(f"  → 键盘 UART RX 侧丢帧合计 {u_rx_err}  vs 上行丢帧(PC 回显已写出但未回) "
              f"{len([s for s in ul_lost if s not in echo_fail_seqs])}")
        if u_rx_err >= len([s for s in ul_lost if s not in echo_fail_seqs]) \
                and u_rx_err > 0:
            print("  ★ 根因指向：键盘 UART RX 采样丢字节（RF 负载下 FIFO 溢出/中断迟延）"
                  "→ 回显命令帧被截断/丢弃，上行帧未生成")
        elif u["u_crc"] > 0:
            print("  → 键盘 UART RX 收到但 CRC 错：帧被线路噪声/采样错破坏")
    else:
        print("\n---- 键盘 UART 接收腿取证 ----\n  固件未含 42B 段（旧固件），无法取证")
    link.stop()


def do_uartleg(count: int = 20000, wait_ms: int = 50) -> None:
    """UART 腿隔离实验（只走 COM13，PC↔键盘，不经 2.4G 射频）。

    动机：soak 的 37 个上行丢包在所有「已测腿」上都干净（键盘 RF tx_drop=1、
    Dongle 收包覆写 ovw=0、EP3 队满丢 qdrop=0、PC 读侧 CRC=0），唯一未取证的
    链路是「PC→键盘 UART 写 / 键盘 UART RX」。本实验用与 soak 完全一致的测试
    方法（一发一收 + 帧内计数值 + 50ms 超时）单独压这条腿：
      下行：PC 写 CMD_GET_VERSION(0x01)（固件 handler 原样回 f->seq）
      上行：键盘经 UART 回 GET_VERSION 应答，seq == 本帧 seq 即判成功。
    任一方向丢字节/丢帧都表现为「超时无应答」；读方向丢字节还会被 CRC 检出。
    判定：若本实验丢包率≈soak 上行丢包率(0.04%) 且呈突发 → 坐实上行丢包在
          UART 腿（WCH-Link 写侧 / 键盘 UART RX）；若≈0 → UART 腿干净，需另找。
    """
    from sk32sim.link import SerialTransport  # noqa: E402
    from sk32sim import proto as P  # noqa: E402

    tr = SerialTransport(KEYBD, BAUD, idle_poll_s=0.001)

    def drain(rounds: int = 8) -> None:
        for _ in range(rounds):
            if not tr.read(0.01):
                break

    # 关心跳 + 关自动休眠（镜像 soak 条件），清残留
    for _ in range(6):
        try:
            tr.write(P.encode(P.CMD_SET_HEARTBEAT, 0x00, 0, bytes((0,))))
            break
        except Exception:  # noqa: BLE001
            time.sleep(0.2)
    time.sleep(0.15)
    for _ in range(3):
        try:
            tr.write(P.encode(P.CMD_SET_CONFIG, 0x01, 0,
                              bytes((P.CFG_KEY_SLEEP_POLICY, 1))))
            break
        except Exception:  # noqa: BLE001
            time.sleep(0.2)
    time.sleep(0.15)
    drain()
    parser = P.FrameParser()   # 复位解析器，隔离配置阶段的残留字节

    sent = acked = write_fail = mismatch = retry = 0
    lost_recs: list = []      # (seq, 注入后经过秒数) —— 重发耗尽仍无应答才记录
    mismatch_recs: list = []
    lat: list = []
    t_all0 = time.monotonic()
    prev_ok = prev_crc = prev_retry = 0

    for i in range(count):
        seq = i & 0xFF
        pl = bytes((i & 0xFF, (i >> 8) & 0xFF, (i >> 16) & 0xFF, (i >> 24) & 0xFF))
        frame = P.encode(P.CMD_GET_VERSION, seq, 0, pl)
        # 一发一收 + 无应答整帧重发（≤RETRY_MAX 次）：收到 seq 匹配的应答才算发送完成
        ok = False
        submitted = False
        t0 = None
        for attempt in range(RETRY_MAX + 1):
            try:
                tr.write(frame)
            except Exception:  # noqa: BLE001
                if attempt < RETRY_MAX:
                    retry += 1
                    continue
                break
            if not submitted:
                submitted = True
                sent += 1
            t0 = time.monotonic()
            deadline = t0 + wait_ms / 1000.0
            while time.monotonic() < deadline:
                data = tr.read(0.003)
                if not data:
                    continue
                for fr in parser.feed(data):
                    if fr.cmd != P.CMD_GET_VERSION:
                        continue
                    if fr.seq == seq:
                        ok = True
                        break
                    # 序号与本帧不符：迟到/重复/错序应答
                    mismatch += 1
                    mismatch_recs.append((i, fr.seq))
                if ok:
                    break
            if ok:
                break
            if attempt < RETRY_MAX:
                retry += 1
        if ok:
            acked += 1
            lat.append((time.monotonic() - t0) * 1000.0)
        else:
            if not submitted:
                write_fail += 1
            lost_recs.append((i, time.monotonic() - t_all0))
        if (i + 1) % 1000 == 0:
            win_ok = acked - prev_ok
            win_crc = parser.crc_errors - prev_crc
            win_retry = retry - prev_retry
            prev_ok, prev_crc, prev_retry = acked, parser.crc_errors, retry
            extra = (f"  [本窗 丢{1000 - win_ok} 重发{win_retry} crc{win_crc}]"
                     if (win_ok < 1000 or win_retry or win_crc) else "")
            print(f"   ...{i + 1}/{count}  往返成功 {acked}  丢 {sent - acked}  "
                  f"写失败 {write_fail}{extra}", flush=True)

    dt = time.monotonic() - t_all0
    lost = sent - acked
    print(f"\n==== UART 腿隔离（COM13，{count} 次一发一收）====")
    print(f"  提交 {sent}  往返成功 {acked}  丢包 {lost}  "
          f"丢包率 {(lost / sent * 100 if sent else 0):.3f}%  写失败 {write_fail}")
    print(f"  序号校验: 应答错序/重复 {mismatch}")
    print(f"  解析器: 收帧 {parser.frames}  CRC 错 {parser.crc_errors}")
    if mismatch_recs:
        print(f"  错序明细 前50: {mismatch_recs[:50]}")
    if lat:
        lat.sort()
        print(f"  往返时延 ms: 平均 {sum(lat) / len(lat):.2f}  "
              f"中位 {lat[len(lat) // 2]:.2f}  P95 {lat[int(len(lat) * 0.95)]:.2f}  "
              f"最大 {lat[-1]:.2f}")
    print(f"  耗时 {dt:.1f}s  速率 {sent / dt:.1f} 帧/s")
    if lost_recs:
        print(f"  丢帧序号 前80: {[s for s, _ in lost_recs][:80]}")
        print("  时间线: " + "  ".join(f"{s}@{t:.1f}s" for s, t in lost_recs[:120]))
    else:
        print("  丢帧序号: （无）")
    tr.close()


def do_dbg() -> None:
    print("==== 两端 diag ====")
    # 键盘：UART1 GET_DIAG
    try:
        ser = open_port(KEYBD)
        fr = xact(ser, proto.CMD_GET_DIAG, b"", 0.9)
        ser.close()
        if fr and len(fr.payload) >= 24:
            _show("键盘", _parse_diag24(fr.payload))
        else:
            print("  [键盘] 无应答")
    except Exception as e:  # noqa: BLE001
        print(f"  [键盘] 读取失败: {e}")
    # Dongle：USB vendor HID
    dd = _dongle_diag_raw()
    if dd:
        _show("Dongle", _parse_diag24(dd))
    else:
        print("  [Dongle] diag 读取失败")


def do_scan() -> None:
    for port, label in ((DONGLE, "Dongle"), (KEYBD, "键盘")):
        for baud in (921600, 115200, 460800, 230400, 9600):
            try:
                ser = serial.Serial(port, baud, bytesize=8, parity="N",
                                    stopbits=1, timeout=0, write_timeout=1.0)
            except Exception as e:  # noqa: BLE001
                print(f"[scan] {label} {port} @{baud} 打开失败 {e}")
                continue
            time.sleep(0.15)
            got = ser.read(4096) if ser.in_waiting else b""
            ser.close()
            if got:
                print(f"[scan] {label} {port} @{baud} 收到 {len(got)}B: {got.hex(' ')}")
            else:
                print(f"[scan] {label} {port} @{baud} 静默")


def do_unpair() -> None:
    for port, label in ((DONGLE, "Dongle"), (KEYBD, "键盘")):
        print(f"[unpair] {label} ...")
        ser = open_port(port)
        print("   ", xact(ser, proto.CMD_UNPAIR, bytes((0xFF,)), 0.7))
        ser.close()


def do_reset() -> None:
    """软件复位两端（CMD_RESET=0x07），清零 diag 计数器。

    计数器实现为「饱和到 0xFFFF」，长跑后会钉死，导致 soak 增量失效；
    复位后计数器从 0 起，soak 增量才有意义。复位会断开 2.4G 链路，
    需随后重新配对（soak/verify 自带配对流程）。
    """
    for port, label in ((DONGLE, "Dongle"), (KEYBD, "键盘")):
        print(f"[reset] {label} ({port}) ...")
        try:
            ser = open_port(port)
            ser.write(proto.encode(proto.CMD_RESET, 0x40, 0, b""))
            ser.flush()
            ser.close()
        except Exception as e:  # noqa: BLE001
            print(f"   {label} 复位写失败: {e}")
        time.sleep(1.0)
    print("[reset] 等待两端重启 (3s) ...")
    time.sleep(3.0)


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else "read"
    if mode == "read":
        dump(DONGLE, "DONGLE")
        dump(KEYBD, "KEYBOARD")
    elif mode == "pair":
        do_pair()
        dump(DONGLE, "DONGLE")
        dump(KEYBD, "KEYBOARD")
    elif mode == "unpair":
        do_unpair()
    elif mode == "reset":
        do_reset()
    elif mode == "raw":
        do_raw(sys.argv[2] if len(sys.argv) > 2 else KEYBD)
    elif mode == "scan":
        do_scan()
    elif mode == "dongle":
        dongle_diag()
    elif mode == "verify":
        do_verify()
    elif mode == "soak":
        do_soak(int(sys.argv[2]) if len(sys.argv) > 2 else 10000)
    elif mode == "dbg":
        do_dbg()
    elif mode == "uartleg":
        do_uartleg(int(sys.argv[2]) if len(sys.argv) > 2 else 20000,
                   int(sys.argv[3]) if len(sys.argv) > 3 else 50)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
