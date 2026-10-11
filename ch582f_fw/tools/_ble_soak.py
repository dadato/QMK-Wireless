"""临时工具：通路 B（BLE HOGP vendor RID5）大样本 VIA 往返 soak，对齐 _diagcheck.py do_soak 口径。

流程：写键盘模组 BLE HOGP vendor 集合（RID5，32B，payload[28..31]=LE32 序号）
      -> 模组 UART EVT_TUNNEL_BLE -> 本脚本按原 payload 回显 CMD_TUNNEL_BLE
      -> 模组 HOGP vendor IN -> 逐帧按序号配对统计成功/丢包（严格一发一收）。

测试发送语义（与 _diagcheck.do_soak 一致）：
  发一帧 -> 只等**本帧序号**的应答（校验响应与顺序），收到才记「发送完成」并进下一帧；
  超过 WAIT_MS 无应答则**整帧重发**（同序号，幂等回显），最多重发 RETRY_MAX 次；
  重发耗尽仍无应答才记丢包。

根因方向判定（重发事件分类，用于区分 BLE 两条腿）：
  腿A = 主机(BLE HID write RID5) -> 模组；腿B = 模组 UART EVT_TUNNEL_BLE -> 本脚本；
  腿C = 本脚本 UART CMD_TUNNEL_BLE 回显 -> 模组；腿D = 模组 BLE HOGP IN notify -> 本脚本。
  某帧首次超时（尚未重发）时若其序号 **已在 dl_seqs**（腿B 已到）→ 腿A 已成功，
  丢的是腿D（模组 notify）；若**不在 dl_seqs** → 腿A 未到模组（主机 BLE 写丢）。

用法: python _ble_soak.py [N]    # 默认 10000
"""
from __future__ import annotations

import sys
import time
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from sk32sim import proto  # noqa: E402
import hid  # noqa: E402

KEYBD = "COM19"   # USB\VID_1A86&PID_8010&MI_01\8&20449A78&3&0001 (键盘侧 WCH-Link SERIAL)
BAUD = 921600
RID = 5          # BLE HOGP vendor 报表 ID（QMK BLE 键盘固定占用 1..4，vendor 用 5）
OFF = 28         # 序号置于 payload[28..31]（LE32），与 do_soak 一致（32 位不回绕，逐帧唯一）

RETRY_MAX = 3    # 整帧重发上限（用户语义：无应答最多重发三次）
WAIT_MS = 800    # 单次等应答窗口；往返时延实测 ~36ms，800ms 足够区分「迟到」与「丢失」

CMD_HID_DIAG = 0x2B   # BLE HID 取证（sk32sim/proto.py 未定义，本地补充；应答 26B）


def read_notify_diag(link):
    """读 CMD_HID_DIAG 的 26B 应答，取模组侧出站 notify 计数。
    返回 (fail, retry_ok, retry_drop) 或 None（读取失败/载荷不足）。"""
    try:
        fr = link.request(CMD_HID_DIAG)
    except Exception:  # noqa: BLE001
        return None
    p = fr.payload
    if len(p) < 26:
        return None

    def u32(off):
        return (p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) | (p[off + 3] << 24))

    return (u32(14), u32(18), u32(22))


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


def _ours(body: bytes) -> bool:
    return len(body) >= OFF + 4 and body[0] == 0x01 and body[1] == 0x00 and body[2] == 0x0D


def find_hid():
    for d in hid.enumerate():
        p = d["path"].decode(errors="replace")
        if d.get("usage_page") == 0xFF60 and "00001812" in p.lower():
            return d
    return None


def main(count: int) -> int:
    from sk32sim.link import BridgeLink, SerialTransport  # noqa: E402
    from sk32sim.client import BridgeClient  # noqa: E402
    from sk32sim import proto as P  # noqa: E402

    link = BridgeLink(SerialTransport(KEYBD, BAUD, idle_poll_s=0.001), baud=BAUD,
                      gap_ms=1.0, log=lambda s: None)
    link.start()
    client = BridgeClient(link)
    time.sleep(0.3)

    print("[ble-soak] 唤醒 + MANUAL + BLE 模式确认 ...")
    for _ in range(8):
        try:
            client.sleep(P.SLEEP_WAKE)
            break
        except Exception:  # noqa: BLE001
            time.sleep(0.3)
    try:
        client.set_sleep_policy(1)
    except Exception:  # noqa: BLE001
        pass
    try:
        client.set_heartbeat(False)
    except Exception:  # noqa: BLE001
        pass
    try:
        client.set_mode(P.MODE_BLE)
    except Exception as e:  # noqa: BLE001
        print("  set_mode BLE:", e)

    dev = find_hid()
    if dev is None:
        print("!! 未找到 BLE HOGP vendor HID（usagePage 0xFF60 / 00001812），退出")
        link.stop()
        return 2
    print(f"HID 集合: {dev['path'].decode(errors='replace')}")

    diag0 = read_notify_diag(link)
    if diag0 is None:
        print("  [notify 取证] 基线读取失败（CMD_HID_DIAG 无应答/载荷<26B）")
    else:
        print(f"  [notify 取证] 基线 fail={diag0[0]} retry_ok={diag0[1]} retry_drop={diag0[2]}")

    evt_n = 0
    dl_seqs: set = set()      # 段B：模组把下行帧经 UART EVT_TUNNEL_BLE 吐出（腿A+B 成功）
    ul_seqs: set = set()      # 段D：本脚本经 BLE HOGP IN 收到回显（腿D 成功）
    ul_t0: dict = {}
    ul_lat: list = []

    def on_event(fr):
        nonlocal evt_n
        if fr.cmd == P.EVT_TUNNEL_BLE:
            body = fr.payload[:32]
            s = seq_of(body)
            if s is not None and _ours(body):
                dl_seqs.add(s)
                evt_n += 1
            try:
                link.send_hid(P.CMD_TUNNEL_BLE, body)
                if s is not None and _ours(body):
                    if len(ul_t0) > 16:
                        ul_t0.clear()
                    ul_t0[s] = time.monotonic()
            except Exception:  # noqa: BLE001
                pass

    link.on_event = on_event

    h = hid.device()
    h.open_path(dev["path"])

    def _rec_in(raw: bytes):
        """记录一次 BLE IN 到达（腿D），返回序号（非本工具帧返回 None）。"""
        rb = raw[1:33] if raw[:1] == bytes([RID]) and len(raw) >= 33 else raw[:32]
        s = seq_of(rb)
        if s is None or not _ours(rb):
            return None
        ul_seqs.add(s)
        t1 = ul_t0.pop(s, None)
        if t1 is not None:
            ul_lat.append((time.monotonic() - t1) * 1000.0)
        return s

    sent = delivered = write_fail = retry = 0
    lat: list = []
    lost_recs: list = []          # 重发耗尽仍无应答：(seq, 注入后秒数)
    retry_recs: list = []         # 需重发才成功：(seq, leg)  leg='D' 腿D丢 / 'A' 腿A丢
    mismatch = 0
    mismatch_recs: list = []
    t_all0 = time.monotonic()
    prev_delivered = prev_retry = prev_lost = 0
    try:
        while True:  # 清残留 IN（同时计入上行到达）
            data = h.read(64, 10)
            if not data:
                break
            _rec_in(bytes(data))

        for i in range(count):
            # 严格「一发一收」：发本帧后只等**它自己的序号**；收到才记「发送完成」，
            # 否则整帧重发（≤RETRY_MAX 次）。首次超时点即时判定丢的是腿A 还是腿D。
            expect = i   # 32 位计数值：逐帧唯一，可与应答序号严格比对
            ok = False
            submitted = False
            first_leg = None
            t0 = None
            for attempt in range(RETRY_MAX + 1):
                while True:  # 清残留：上一帧/上一次尝试迟到的应答，仅计入上行到达
                    data = h.read(64, 1)
                    if not data:
                        break
                    _rec_in(bytes(data))
                try:
                    wn = h.write(bytes([RID]) + mk_frame(i))
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
                deadline = t0 + WAIT_MS / 1000.0
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
                    if first_leg is None:
                        # 首次超时即时定界：此时 800ms 已远超实测 ~36ms 往返，
                        # 若腿A/B 成功则 EVT 早已到达，故 dl_seqs 成员资格可信。
                        first_leg = 'D' if i in dl_seqs else 'A'
            if not submitted:
                write_fail += 1   # 本帧所有尝试均写失败 → 未提交，不计入射频丢包
                continue
            if ok:
                delivered += 1
                lat.append((time.monotonic() - t0) * 1000.0)
                if first_leg is not None:
                    retry_recs.append((i, first_leg))
            else:
                lost_recs.append((i, time.monotonic() - t_all0))
            if (i + 1) % 100 == 0:
                win_ok = delivered - prev_delivered
                win_retry = retry - prev_retry
                win_lost = len(lost_recs) - prev_lost
                prev_delivered, prev_retry, prev_lost = delivered, retry, len(lost_recs)
                extra = ""
                if win_retry or win_lost:
                    extra = f"  [本窗 重发{win_retry} 新丢{win_lost}]"
                print(f"  ...{i + 1}/{count}  往返成功 {delivered}  丢 {sent - delivered}  "
                      f"重发 {retry}  写失败 {write_fail}{extra}", flush=True)
    finally:
        h.close()
    dt = time.monotonic() - t_all0
    lost = sent - delivered
    print(f"\n==== VIA BLE 往返丢包统计（{count} 次）====")
    print(f"  提交 {sent}  往返成功 {delivered}  丢包 {lost}  "
          f"丢包率 {(lost / sent * 100 if sent else 0):.2f}%  HID写失败 {write_fail}")
    print(f"  无应答整帧重发 {retry} 次（≤{RETRY_MAX} 次/帧；重发耗尽才记丢包）")
    print(f"  序号校验: 应答错序/重复 {mismatch}")
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

    # ---- 根因方向拆分：重发事件按腿归因 ----
    nA = sum(1 for _s, leg in retry_recs if leg == 'A')
    nD = sum(1 for _s, leg in retry_recs if leg == 'D')
    print("\n---- 根因方向拆分（需重发才成功的帧）----")
    print(f"  重发事件 {len(retry_recs)} 帧:")
    print(f"    腿A 主机 BLE 写丢（首超时该帧未到模组）    {nA}")
    print(f"    腿D 模组 notify 丢（首超时该帧已到模组）   {nD}")
    if retry_recs:
        print(f"    明细 (seq,leg) 前50: {retry_recs[:50]}")

    # ---- 逐丢帧定界（重发耗尽）----
    if lost_recs:
        lost_A = [s for s, _t in lost_recs if s not in dl_seqs]
        lost_D = [s for s, _t in lost_recs if s in dl_seqs]
        print("\n---- 丢帧明细（seq@秒，重发耗尽）----")
        print(f"  腿A 丢（未到模组）        {len(lost_A)}: {lost_A}")
        print(f"  腿D 丢（到模组未回主机）  {len(lost_D)}: {lost_D}")
        print("  时间线: " + "  ".join(f"{s}@{t:.1f}s" for s, t in lost_recs))
    print(f"  到达统计: 腿A/B 到模组 {len(dl_seqs)}  腿D 回主机 {len(ul_seqs)}  "
          f"下行EVT事件 {evt_n}")

    # ---- 模组侧出站 notify 取证：定界腿D 归属（模组 notify 失败 vs 主机丢弃）----
    diag1 = read_notify_diag(link)
    print("\n---- 模组侧出站 notify 取证（CMD_HID_DIAG）----")
    if diag0 is None or diag1 is None:
        print("  取证读取失败：无法定界（需固件含 26B port_hid_diag）")
    else:
        d_fail = diag1[0] - diag0[0]
        d_ok = diag1[1] - diag0[1]
        d_drop = diag1[2] - diag0[2]
        print(f"  基线 fail={diag0[0]} retry_ok={diag0[1]} retry_drop={diag0[2]}")
        print(f"  终值 fail={diag1[0]} retry_ok={diag1[1]} retry_drop={diag1[2]}")
        print(f"  增量 fail={d_fail} retry_ok={d_ok} retry_drop={d_drop}  "
              f"（本段需重发 {len(retry_recs)} 帧）")
        if d_fail == 0 and len(retry_recs) > 0:
            print("  判定：腿D 丢帧发生在**主机侧**（模组 HidDev_Report 全部成功，无瞬态失败）")
        elif d_fail > 0:
            print(f"  判定：腿D 丢帧发生在**模组侧**（HidDev_Report 失败 {d_fail} 次；"
                  f"延迟重发挽回 {d_ok} 次/仍失败 {d_drop} 次）")
        else:
            print("  判定：本段无重发事件，无异常见象")
    link.stop()
    return 0


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
    raise SystemExit(main(n))
