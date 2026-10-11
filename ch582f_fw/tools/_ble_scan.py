"""临时工具：用 BLE 广播扫描器列出当前可见的 BLE 外设（聚焦 SK32 键盘），打印当前地址/RSSI。
用法: python _ble_scan.py [name_filter] [seconds]
"""
import sys
import time

from winrt.windows.devices.bluetooth.advertisement import (
    BluetoothLEAdvertisementWatcher,
    BluetoothLEScanningMode,
)


def fmt_addr(mac) -> str:
    try:
        return ":".join(f"{x:02X}" for x in mac.to_bytes(6, "little"))
    except Exception:  # noqa: BLE001
        return str(mac)


def main(flt: str, secs: float) -> int:
    seen: dict = {}
    watcher = BluetoothLEAdvertisementWatcher()
    watcher.scanning_mode = BluetoothLEScanningMode.ACTIVE

    def on_received(sender, args):
        adv = args.advertisement
        name = adv.local_name or ""
        addr = getattr(args, "bluetooth_address", None)
        key = fmt_addr(addr)
        if key not in seen:
            seen[key] = (name, getattr(args, "rssi", None), getattr(args, "is_connectable", None))
            print(f"  + {key}  name={name!r}  rssi={getattr(args,'rssi',None)}  "
                  f"conn={getattr(args,'is_connectable',None)}", flush=True)

    watcher.add_received(on_received)
    print(f"[scan] start ({secs}s) filter={flt!r}")
    watcher.start()
    time.sleep(secs)
    watcher.stop()
    print(f"[scan] {len(seen)} unique advertisers")
    print("--- matches ---")
    for key, (name, rssi, conn) in seen.items():
        if not flt or flt.lower() in (name or "").lower():
            print(f"  {key}  {name!r}  rssi={rssi} conn={conn}")
    return 0


if __name__ == "__main__":
    f = sys.argv[1] if len(sys.argv) > 1 else "SK32"
    s = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
    raise SystemExit(main(f, s))
