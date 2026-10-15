/* 桥帧协议（键盘侧 UART1）—— 与 core/bridge_proto.h / bridge_uart.c / tools/sk32sim/proto.py 对齐
 *
 * 帧格式（CRC 小端，覆盖 LEN..PAYLOAD，不含 SOF）：
 *   +--------+-----+-----+-----+-------+---------+----------+
 *   | SOF    | LEN | CMD | SEQ | FLAGS | PAYLOAD | CRC16    |
 *   | AA 55  | 1B  | 1B  | 1B  |  1B   |  N B    | 2B（LE） |
 *   +--------+-----+-----+-----+-------+---------+----------+
 *
 * 接口风格沿用 ota_web/js/frame.js（IIFE + global 导出 + module.exports 供 Node 自检）。
 */
(function (global) {
  'use strict';

  var crc16 = global.DbgCrc ? global.DbgCrc.crc16 : require('./crc16.js').crc16;

  var SOF0 = 0xAA;
  var SOF1 = 0x55;
  var SOF = Uint8Array.of(SOF0, SOF1);
  var MAX_PAYLOAD = 64;
  var HDR_LEN = 4;
  var CRC_LEN = 2;
  var FRAME_OVERHEAD = 2 + HDR_LEN + CRC_LEN; /* 8 */

  var FLAG_ACK_REQ = 0x01;
  var FLAG_ACK = 0x02;
  var FLAG_ERR = 0x04;

  /* ---------------------------------------------------------------------
   * 命令码：SK32 → 桥（bridge_types.h）
   * ------------------------------------------------------------------- */
  var CMD = {
    GET_VERSION: 0x01,
    GET_STATUS: 0x02,
    SET_MODE: 0x03,
    SET_PROFILE: 0x04,
    PAIR_START: 0x05,
    UNPAIR: 0x06,
    RESET: 0x07,
    ENTER_BOOT: 0x08,
    SET_CONFIG: 0x09,
    GET_MAC: 0x0A,
    SET_TEST_MODE: 0x0B,
    GET_MODE: 0x0C,
    SET_HEARTBEAT: 0x0D,
    GET_WIRELESS_STATE: 0x0E,
    GET_BLE_SLOTS: 0x0F,

    HID_KEYBOARD: 0x10,
    HID_NKRO: 0x11,
    HID_CONSUMER: 0x12,
    HID_MOUSE: 0x13,
    HID_SYSTEM: 0x14,
    HID_RELEASE: 0x15,

    TUNNEL_USB: 0x18,
    TUNNEL_BLE: 0x19,
    NV_WRITE: 0x1A,
    NV_READ: 0x1B,
    SET_BATTERY: 0x1C, /* 下发电量：payload [pct u8][flags u8]（bit0 = 充电中） */
    SLEEP: 0x1D,
    GET_ADC: 0x1E,
    SET_NAME: 0x1F,
    SET_USB_ID: 0x20,

    FW_BEGIN: 0x21,
    FW_DATA: 0x22,
    FW_END: 0x23,
    FW_ABORT: 0x24,
    SLEEP_CFG: 0x25,  /* SLEEP 时间配置：payload [idle_s LE16][deep_s LE16]，单位 s */
    BATTERY_CFG: 0x26, /* 电量上报配置：payload [enable][on_change][period_s]，各 1B（仅 LOCAL 源有 EVT_BATTERY） */

    GET_DIAG: 0x2A,   /* 2.4G 诊断快照（排障用，不在市售模组指令集内；应答 24B） */
    HID_DIAG: 0x2B,   /* BLE HID 服务注册取证（排障用；应答 26B） */

    PING: 0x7F
  };

  /* ---------------------------------------------------------------------
   * 事件码：桥 → SK32
   * ------------------------------------------------------------------- */
  var EVT = {
    STATE: 0x80,
    LEDS: 0x81,
    BATTERY: 0x82,
    PAIR_RESULT: 0x83,
    ERROR: 0x84,
    TUNNEL_USB: 0x85,
    TUNNEL_BLE: 0x86,
    HOST_POWER: 0x87,
    PONG: 0x7F
  };

  /* 事件区间（bridge_proto.h：0x80..0x87） */
  var EVT_MIN = 0x80;
  var EVT_MAX = 0x87;

  var CMD_NAMES = {
    0x01: 'GET_VERSION', 0x02: 'GET_STATUS', 0x03: 'SET_MODE', 0x04: 'SET_PROFILE',
    0x05: 'PAIR_START', 0x06: 'UNPAIR', 0x07: 'RESET', 0x08: 'ENTER_BOOT',
    0x09: 'SET_CONFIG', 0x0A: 'GET_MAC', 0x0B: 'SET_TEST_MODE',
    0x0C: 'GET_MODE', 0x0D: 'SET_HEARTBEAT', 0x0E: 'GET_WIRELESS_STATE',
    0x0F: 'GET_BLE_SLOTS',
    0x10: 'HID_KEYBOARD', 0x11: 'HID_NKRO', 0x12: 'HID_CONSUMER', 0x13: 'HID_MOUSE',
    0x14: 'HID_SYSTEM', 0x15: 'HID_RELEASE',
    0x18: 'TUNNEL_USB', 0x19: 'TUNNEL_BLE', 0x1A: 'NV_WRITE', 0x1B: 'NV_READ',
    0x1C: 'SET_BATTERY', 0x1D: 'SLEEP', 0x1E: 'GET_ADC', 0x1F: 'SET_NAME',
    0x20: 'SET_USB_ID', 0x21: 'FW_BEGIN', 0x22: 'FW_DATA', 0x23: 'FW_END',
    0x24: 'FW_ABORT', 0x25: 'SLEEP_CFG', 0x26: 'BATTERY_CFG', 0x2A: 'GET_DIAG', 0x2B: 'HID_DIAG', 0x7F: 'PING'
  };

  var EVENT_NAMES = {
    0x80: 'EVT_STATE', 0x81: 'EVT_LEDS', 0x82: 'EVT_BATTERY', 0x83: 'EVT_PAIR_RESULT',
    0x84: 'EVT_ERROR', 0x85: 'EVT_TUNNEL_USB', 0x86: 'EVT_TUNNEL_BLE',
    0x87: 'EVT_HOST_POWER', 0x7F: 'EVT_PONG'
  };

  /* 模式（bridge_mode.h） */
  var MODE = {
    USB: 0, BLE: 1, BLE_PAIR: 2, '2G4': 3, '2G4_PAIR': 4, OTA: 5, SLEEP: 6
  };
  var MODE_NAMES = {
    0: 'USB', 1: 'BLE', 2: 'BLE_PAIR', 3: '2G4', 4: '2G4_PAIR', 5: 'OTA', 6: 'SLEEP'
  };
  var MAX_BLE_PROFILE = 4;

  /* SET_CONFIG 键（bridge_cfg.h） */
  var CFG_KEY = {
    REPORT_PERIOD: 0x01, LOG_LEVEL: 0x02, ADV_NAME_MAC: 0x03,
    CONN_INTERVAL: 0x04, BATTERY_SRC: 0x05, SLEEP_POLICY: 0x06
  };
  var CFG_KEY_NAMES = {
    0x01: 'REPORT_PERIOD', 0x02: 'LOG_LEVEL', 0x03: 'ADV_NAME_MAC',
    0x04: 'CONN_INTERVAL', 0x05: 'BATTERY_SRC', 0x06: 'SLEEP_POLICY'
  };

  /* 名称目标 / 电量来源 / 睡眠分级 / 睡眠策略 / 射频通道（bridge_types.h） */
  var NAME_TARGET = { BLE: 0x00, USB: 0x01 };
  var BATT_SRC = { AUTO: 0, LOCAL: 1, EXTERNAL: 2 };
  var SLEEP = { WAKE: 0, IDLE: 1, DEEP: 2 };
  var SLEEP_POLICY = { AUTO: 0, MANUAL: 1 };
  var RADIO = { NONE: 0, BLE: 1, '2G4': 2 };
  var RADIO_NAMES = { 0: 'NONE', 1: 'BLE', 2: '2G4' };
  /* 主机电源状态（bridge_types.h：EVT_HOST_POWER / BRIDGE_RADIO_PKT_HOST） */
  var HOST = { AWAKE: 0, SUSPEND: 1 };
  var HOST_NAMES = { 0: 'AWAKE', 1: 'SUSPEND' };
  /* 电量载荷标志位（bridge_types.h：EVT_BATTERY / CMD_SET_BATTERY 的 flags 字节） */
  var BATT_FLAG = { CHARGING: 0x01 };
  /* 无线综合状态（bridge_types.h：CMD_GET_WIRELESS_STATE 应答单字节） */
  var WL = {
    USB: 0, IDLE: 1, BLE_PAIRING: 2, '2G4_PAIRING': 3, BLE_LINKED: 4,
    '2G4_LINKED': 5, BLE_DISCONN: 6, '2G4_DISCONN': 7, OTA: 8, SLEEP: 9
  };
  var WL_NAMES = {
    0: 'USB', 1: 'IDLE', 2: 'BLE_PAIRING', 3: '2G4_PAIRING', 4: 'BLE_LINKED',
    5: '2G4_LINKED', 6: 'BLE_DISCONN', 7: '2G4_DISCONN', 8: 'OTA', 9: 'SLEEP'
  };
  var TUNNEL_USB_LEN = 64;
  var TUNNEL_BLE_LEN = 32;
  var TEST_CH_MAX = 39;
  var TEST_POWER_MAX = 3;

  /* 用户 NV（bridge_config.h「应用 NV 全槽分配」） */
  var NV_UNIT = 32;
  var NV_USER_UNITS = 46;

  /* HID 报文长度（bridge_types.h） */
  var REPORT_LEN = {
    0x10: 8, 0x11: 16, 0x12: 2, 0x13: 4, 0x14: 1, 0x15: 0
  };

  /* 错误码 EVT_ERROR / ERR 帧（bridge_types.h） */
  var ECODE = {
    CRC: 0x01, UART_LOST: 0x02, RADIO: 0x03, PAIR: 0x04,
    ACK_TIMEOUT: 0x05, PARAM: 0x06, STATE: 0x07, ROLLOVER: 0x08
  };
  var ECODE_NAMES = {
    0x01: 'CRC', 0x02: 'UART_LOST', 0x03: 'RADIO', 0x04: 'PAIR',
    0x05: 'ACK_TIMEOUT', 0x06: 'PARAM', 0x07: 'STATE', 0x08: 'ROLLOVER'
  };
  /* 返回码（负值取绝对值索引） */
  var BRIDGE_ERR = {
    1: 'BUSY', 2: 'GAP', 3: 'DUP', 4: 'PARAM', 5: 'STATE',
    6: 'VBUS', 7: 'UNSUPPORTED', 8: 'IO', 9: 'CRC'
  };

  function hex2(n) { return (n & 0xFF).toString(16).toUpperCase().padStart(2, '0'); }

  function cmdName(cmd) {
    return CMD_NAMES[cmd] || EVENT_NAMES[cmd] || ('0x' + hex2(cmd));
  }

  function eventName(cmd) {
    return EVENT_NAMES[cmd] || cmdName(cmd);
  }

  function modeName(mode) {
    return MODE_NAMES[mode] || ('?' + mode);
  }

  function radioName(radio) {
    return RADIO_NAMES[radio] || ('?' + radio);
  }

  function hostName(state) {
    return HOST_NAMES[state] || ('?' + state);
  }

  function wlStateName(state) {
    return WL_NAMES[state] || ('?' + state);
  }

  function errCodeName(code) {
    return ECODE_NAMES[code] || ('0x' + hex2(code));
  }

  /** 负值返回码 / 正值错误码 → 可读名（镜像 frame.bridgeErrName） */
  function bridgeErrName(status) {
    if (status === 0) { return 'OK'; }
    var signed = (status > 127) ? status - 256 : status;
    if (signed < 0) {
      var n = BRIDGE_ERR[-signed];
      return n ? ('BRIDGE_ERR_' + n) : ('status=0x' + hex2(status));
    }
    var e = ECODE_NAMES[signed];
    return e ? ('ECODE_' + e) : ('status=' + status);
  }

  function BridgeError(message, code) {
    this.name = 'BridgeError';
    this.message = message;
    this.code = (code === undefined) ? null : code;
    if (Error.captureStackTrace) { Error.captureStackTrace(this, BridgeError); }
  }
  BridgeError.prototype = Object.create(Error.prototype);
  BridgeError.prototype.constructor = BridgeError;

  function BridgeTimeout(message) {
    this.name = 'BridgeTimeout';
    this.message = message || 'timeout';
    if (Error.captureStackTrace) { Error.captureStackTrace(this, BridgeTimeout); }
  }
  BridgeTimeout.prototype = Object.create(Error.prototype);
  BridgeTimeout.prototype.constructor = BridgeTimeout;

  /** 组帧：payload 超限抛错（镜像 bridge_uart_encode / proto.encode） */
  function encode(cmd, seq, flags, payload) {
    var p = payload || new Uint8Array(0);
    if (p.length > MAX_PAYLOAD) {
      throw new BridgeError('payload ' + p.length + 'B 超过上限 ' + MAX_PAYLOAD + 'B');
    }
    var out = new Uint8Array(FRAME_OVERHEAD + p.length);
    out[0] = SOF0;
    out[1] = SOF1;
    out[2] = p.length;
    out[3] = cmd & 0xFF;
    out[4] = seq & 0xFF;
    out[5] = flags & 0xFF;
    out.set(p, 6);
    var crc = crc16(out.subarray(2, 6 + p.length));
    out[6 + p.length] = crc & 0xFF;
    out[7 + p.length] = (crc >> 8) & 0xFF;
    return out;
  }

  function Frame(cmd, seq, flags, payload) {
    this.cmd = cmd;
    this.seq = seq;
    this.flags = flags;
    this.payload = payload || new Uint8Array(0);
  }
  Frame.prototype.isAck = function () { return (this.flags & FLAG_ACK) !== 0; };
  Frame.prototype.isErr = function () { return (this.flags & FLAG_ERR) !== 0; };
  Frame.prototype.isEvent = function () { return this.cmd >= EVT_MIN && this.cmd <= EVT_MAX; };
  Frame.prototype.errCode = function () { return this.payload.length ? this.payload[0] : 0; };
  Frame.prototype.name = function () { return cmdName(this.cmd); };
  Frame.prototype.hex = function () {
    var s = [];
    for (var i = 0; i < this.payload.length; i++) { s.push(hex2(this.payload[i])); }
    return s.join(' ');
  };

  /** 帧类型标签：TX/ACK/ERR/EVT/RX（供监视器与日志使用） */
  Frame.prototype.kind = function () {
    if (this.isErr()) { return 'ERR'; }
    if (this.isEvent()) { return 'EVT'; }
    if (this.isAck()) { return 'ACK'; }
    return 'RX';
  };

  /** 增量解析器：镜像 bridge_uart.c 的 RX 状态机容错（AA AA 55 / LEN>64 丢弃 / CRC 不符重同步）
   *  @param {(raw: Uint8Array) => void} [onCrcError] CRC 错误回调（含原始字节，用于高亮）
   */
  function Parser(onCrcError) {
    this.buf = [];
    this.crcErrors = 0;
    this.frames = 0;
    this.onCrcError = onCrcError || null;
  }
  Parser.prototype.feed = function (data) {
    var out = [];
    for (var i = 0; i < data.length; i++) { this.buf.push(data[i] & 0xFF); }
    for (;;) {
      var buf = this.buf;
      var start = -1;
      for (var k = 0; k + 1 < buf.length; k++) {
        if (buf[k] === SOF0 && buf[k + 1] === SOF1) { start = k; break; }
      }
      if (start < 0) {
        /* 保留可能的半个 SOF（尾部单字节 AA） */
        this.buf = (buf.length && buf[buf.length - 1] === SOF0) ? [SOF0] : [];
        return out;
      }
      if (start > 0) { this.buf.splice(0, start); }
      if (this.buf.length < 2 + HDR_LEN) { return out; }
      var len = this.buf[2];
      if (len > MAX_PAYLOAD) { this.buf.shift(); continue; }
      var total = FRAME_OVERHEAD + len;
      if (this.buf.length < total) { return out; }
      var body = this.buf.slice(2, 2 + HDR_LEN + len);
      var calc = crc16(Uint8Array.from(body));
      var rx = this.buf[total - 2] | (this.buf[total - 1] << 8);
      if (calc === rx) {
        out.push(new Frame(body[1], body[2], body[3], Uint8Array.from(body.slice(4))));
        this.frames++;
        this.buf.splice(0, total);
      } else {
        this.crcErrors++;
        if (this.onCrcError) { this.onCrcError(Uint8Array.from(this.buf.slice(0, total))); }
        this.buf.shift(); /* 重同步 */
      }
    }
  };

  /* 工具：hex 字符串 ↔ 字节数组 */
  function hexToBytes(hex) {
    var s = String(hex || '').replace(/[\s,:\-]/g, '');
    if (!s) { return new Uint8Array(0); }
    if (s.length % 2 !== 0 || /[^0-9a-fA-F]/.test(s)) {
      throw new BridgeError('非法 hex：' + hex);
    }
    var out = new Uint8Array(s.length / 2);
    for (var i = 0; i < out.length; i++) { out[i] = parseInt(s.substr(i * 2, 2), 16); }
    return out;
  }
  function bytesToHex(bytes, sep) {
    var s = [];
    for (var i = 0; i < bytes.length; i++) { s.push(hex2(bytes[i])); }
    return s.join(sep === undefined ? ' ' : sep);
  }

  var api = {
    SOF0: SOF0, SOF1: SOF1, SOF: SOF,
    MAX_PAYLOAD: MAX_PAYLOAD, HDR_LEN: HDR_LEN, CRC_LEN: CRC_LEN,
    FRAME_OVERHEAD: FRAME_OVERHEAD,
    FLAG_ACK_REQ: FLAG_ACK_REQ, FLAG_ACK: FLAG_ACK, FLAG_ERR: FLAG_ERR,
    CMD: CMD, EVT: EVT, EVT_MIN: EVT_MIN, EVT_MAX: EVT_MAX,
    CMD_NAMES: CMD_NAMES, EVENT_NAMES: EVENT_NAMES,
    MODE: MODE, MODE_NAMES: MODE_NAMES, MAX_BLE_PROFILE: MAX_BLE_PROFILE,
    CFG_KEY: CFG_KEY, CFG_KEY_NAMES: CFG_KEY_NAMES,
    NAME_TARGET: NAME_TARGET, BATT_SRC: BATT_SRC, SLEEP: SLEEP,
    SLEEP_POLICY: SLEEP_POLICY, RADIO: RADIO, RADIO_NAMES: RADIO_NAMES,
    HOST: HOST, HOST_NAMES: HOST_NAMES,
    BATT_FLAG: BATT_FLAG,
    WL: WL, WL_NAMES: WL_NAMES,
    TUNNEL_USB_LEN: TUNNEL_USB_LEN, TUNNEL_BLE_LEN: TUNNEL_BLE_LEN,
    TEST_CH_MAX: TEST_CH_MAX, TEST_POWER_MAX: TEST_POWER_MAX,
    NV_UNIT: NV_UNIT, NV_USER_UNITS: NV_USER_UNITS,
    REPORT_LEN: REPORT_LEN,
    ECODE: ECODE, ECODE_NAMES: ECODE_NAMES, BRIDGE_ERR: BRIDGE_ERR,
    cmdName: cmdName, eventName: eventName, modeName: modeName, radioName: radioName,
    hostName: hostName,
    wlStateName: wlStateName,
    errCodeName: errCodeName, bridgeErrName: bridgeErrName,
    BridgeError: BridgeError, BridgeTimeout: BridgeTimeout,
    encode: encode, Frame: Frame, Parser: Parser,
    hexToBytes: hexToBytes, bytesToHex: bytesToHex
  };
  global.DbgFrame = api;
  if (typeof module === 'object' && module.exports) { module.exports = api; }
})(typeof window !== 'undefined' ? window : globalThis);
