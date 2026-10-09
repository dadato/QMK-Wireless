"""SK32 模拟器（COM 口替代 SK32）—— CH582F 透传桥测试工具包

用途：
  - 在 PC 上通过 USB-TTL 串口与键盘侧 CH582F 模组（UART1 921600 8N1）通信，
    模拟 SK32 的全部协议行为：心跳 PING、应答 ACK、事件解析、全命令下发。
  - 固件侧协议定义出处（单一事实来源，改动必须同步）：
      core/bridge_proto.h     帧格式 / 标志
      core/bridge_types.h     命令码 / 事件码 / 错误码
      core/bridge_cfg.h       SET_CONFIG 键
      core/bridge_mode.h      模式枚举
      core/bridge_test.h      定频测试参数范围
    `python -m sk32sim.proto` 或 selftest 会校验关键常量与头文件一致。

用法：
  python -m sk32sim.selftest               # 无硬件自测（虚拟链路 + 模拟设备）
  python -m sk32sim.cli --port COM7 ...    # 真机测试（见 cli.py --help）
"""

__version__ = "0.1.0"
