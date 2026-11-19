/* 桥帧协议（键盘侧 UART1）—— 与 core/bridge_proto.h / bridge_uart.c / tools/sk32sim/proto.py 对齐
 *
 * 帧格式（CRC 小端，覆盖 LEN..PAYLOAD，不含 SOF）：
 *   +--------+-----+-----+-----+-------+---------+----------+
 *   | SOF    | LEN | CMD | SEQ | FLAGS | PAYLOAD | CRC16    |
 *   | AA 55  | 1B  | 1B  | 1B  |  1B   |  N B    | 2B（LE） |
 *   +--------+-----+-----+-----+-------+---------+----------+
 */
(function (global) {
  'use strict';

  var crc16 = global.OtaCrc ? global.OtaCrc.crc16 : require('./crc16.js').crc16;

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

  /* 命令码（bridge_types.h）—— 仅列本应用用到的 */
  var CMD = {
    GET_VERSION: 0x01,
    GET_STATUS: 0x02,
    RESET: 0x07,
    ENTER_BOOT: 0x08,
    PING: 0x7F,
    FW_BEGIN: 0x21,
    FW_DATA: 0x22,
    FW_END: 0x23,
    FW_ABORT: 0x24
  };

  /* 事件码：桥 → SK32 */
  var EVT = {
    STATE: 0x80,
    LEDS: 0x81,
    PAIR_RESULT: 0x83,
    ERROR: 0x84,
    PONG: 0x7F
  };

  var CMD_NAMES = {
    0x01: 'GET_VERSION', 0x02: 'GET_STATUS', 0x08: 'ENTER_BOOT',
    0x21: 'FW_BEGIN', 0x22: 'FW_DATA',
    0x23: 'FW_END', 0x24: 'FW_ABORT', 0x7F: 'PING',
    0x80: 'EVT_STATE', 0x81: 'EVT_LEDS', 0x83: 'EVT_PAIR_RESULT',
    0x84: 'EVT_ERROR', 0x85: 'EVT_TUNNEL_USB', 0x86: 'EVT_TUNNEL_BLE'
  };

  /* 错误码（bridge_types.h）：负值为内部返回码；正值为 EVT_ERROR / ERR 帧码 */
  var ECODE = {
    0x01: 'CRC', 0x02: 'UART_LOST', 0x03: 'RADIO', 0x04: 'PAIR',
    0x05: 'ACK_TIMEOUT', 0x06: 'PARAM', 0x07: 'STATE', 0x08: 'ROLLOVER'
  };
  var BRIDGE_ERR = {
    1: 'BUSY', 2: 'GAP', 3: 'DUP', 4: 'PARAM', 5: 'STATE',
    6: 'VBUS', 7: 'UNSUPPORTED', 8: 'IO', 9: 'CRC'
  };

  function cmdName(cmd) {
    return CMD_NAMES[cmd] || ('0x' + cmd.toString(16).toUpperCase().padStart(2, '0'));
  }

  /** HID 应答 status 字节 → 可读名（见 SPEC §5.5：负值取反或 ABS） */
  function bridgeErrName(status) {
    if (status === 0) { return 'OK'; }
    var signed = (status > 127) ? status - 256 : status;
    if (signed < 0) {
      var n = BRIDGE_ERR[-signed];
      return n ? ('BRIDGE_ERR_' + n) : ('status=0x' + status.toString(16).toUpperCase());
    }
    var e = ECODE[signed];
    return e ? ('ECODE_' + e) : ('status=' + status);
  }

  function errCodeName(code) {
    return ECODE[code] || ('0x' + code.toString(16).toUpperCase().padStart(2, '0'));
  }

  function OtaError(message, code) {
    this.name = 'OtaError';
    this.message = message;
    this.code = (code === undefined) ? null : code;
    if (Error.captureStackTrace) { Error.captureStackTrace(this, OtaError); }
  }
  OtaError.prototype = Object.create(Error.prototype);
  OtaError.prototype.constructor = OtaError;

  function OtaTimeout(message) {
    this.name = 'OtaTimeout';
    this.message = message || 'timeout';
    if (Error.captureStackTrace) { Error.captureStackTrace(this, OtaTimeout); }
  }
  OtaTimeout.prototype = Object.create(Error.prototype);
  OtaTimeout.prototype.constructor = OtaTimeout;

  /** 组帧：payload 超限抛错 */
  function encode(cmd, seq, flags, payload) {
    var p = payload || new Uint8Array(0);
    if (p.length > MAX_PAYLOAD) {
      throw new OtaError('payload ' + p.length + 'B 超过上限 ' + MAX_PAYLOAD + 'B');
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
  Frame.prototype.isEvent = function () { return this.cmd >= 0x80 && this.cmd <= 0x86; };
  Frame.prototype.errCode = function () { return this.payload.length ? this.payload[0] : 0; };
  Frame.prototype.name = function () { return cmdName(this.cmd); };
  Frame.prototype.hex = function () {
    var s = [];
    for (var i = 0; i < this.payload.length; i++) {
      s.push(this.payload[i].toString(16).toUpperCase().padStart(2, '0'));
    }
    return s.join(' ');
  };

  /** 增量解析器：镜像 bridge_uart.c 的 RX 状态机容错（AA AA 55 / LEN>64 丢弃 / CRC 不符重同步） */
  function Parser() {
    this.buf = [];
    this.crcErrors = 0;
    this.frames = 0;
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
        this.buf.shift(); /* 重同步 */
      }
    }
  };

  var api = {
    SOF0: SOF0, SOF1: SOF1, SOF: SOF,
    MAX_PAYLOAD: MAX_PAYLOAD, HDR_LEN: HDR_LEN, CRC_LEN: CRC_LEN,
    FRAME_OVERHEAD: FRAME_OVERHEAD,
    FLAG_ACK_REQ: FLAG_ACK_REQ, FLAG_ACK: FLAG_ACK, FLAG_ERR: FLAG_ERR,
    CMD: CMD, EVT: EVT, CMD_NAMES: CMD_NAMES, ECODE: ECODE, BRIDGE_ERR: BRIDGE_ERR,
    cmdName: cmdName, errCodeName: errCodeName, bridgeErrName: bridgeErrName,
    OtaError: OtaError, OtaTimeout: OtaTimeout,
    encode: encode, Frame: Frame, Parser: Parser
  };
  global.OtaFrame = api;
  if (typeof module === 'object' && module.exports) { module.exports = api; }
})(typeof window !== 'undefined' ? window : globalThis);
