/* 载荷解码 —— GET_STATUS/GET_MODE/GET_VERSION/GET_MAC/GET_ADC/GET_DIAG/HID_DIAG/EVT_STATE/EVT_LEDS/
 * EVT_BATTERY/EVT_PAIR_RESULT/EVT_ERROR/EVT_HOST_POWER/EVT_TUNNEL_USB|BLE/NV_READ/
 * SET_CONFIG 等（SPEC §4.6/§4.9/§4.10）。
 *
 * 依据 tools/sk32sim/client.py 的字段语义实现；所有函数对短载荷容错（缺失字节补 0）。
 */
(function (global) {
  'use strict';

  var F = global.DbgFrame || require('./frame.js');

  /* HID usage → 可读名（US 布局常用子集，便于监视器摘要） */
  var KEY_NAMES = { 0x00: '' };
  (function () {
    var letters = 'abcdefghijklmnopqrstuvwxyz';
    for (var i = 0; i < 26; i++) { KEY_NAMES[0x04 + i] = letters.charAt(i); }
    var digits = '123456789';
    for (var d = 0; d < 9; d++) { KEY_NAMES[0x1E + d] = digits.charAt(d); }
    KEY_NAMES[0x27] = '0';
    KEY_NAMES[0x28] = 'Enter'; KEY_NAMES[0x29] = 'Esc'; KEY_NAMES[0x2A] = 'Backspace';
    KEY_NAMES[0x2B] = 'Tab'; KEY_NAMES[0x2C] = 'Space'; KEY_NAMES[0x2D] = '-';
    KEY_NAMES[0x2E] = '='; KEY_NAMES[0x2F] = '['; KEY_NAMES[0x30] = ']';
    KEY_NAMES[0x31] = '\\'; KEY_NAMES[0x33] = ';'; KEY_NAMES[0x34] = "'";
    KEY_NAMES[0x35] = '`'; KEY_NAMES[0x36] = ','; KEY_NAMES[0x37] = '.'; KEY_NAMES[0x38] = '/';
    KEY_NAMES[0x39] = 'Caps'; KEY_NAMES[0xE0] = 'LCtrl'; KEY_NAMES[0xE1] = 'LShift';
    KEY_NAMES[0xE2] = 'LAlt'; KEY_NAMES[0xE3] = 'LGui'; KEY_NAMES[0xE4] = 'RCtrl';
    KEY_NAMES[0xE5] = 'RShift'; KEY_NAMES[0xE6] = 'RAlt'; KEY_NAMES[0xE7] = 'RGui';
  })();
  function keyName(u) {
    if (Object.prototype.hasOwnProperty.call(KEY_NAMES, u)) {
      var n = KEY_NAMES[u];
      return n ? (n + '(' + hex2(u) + ')') : ('0x' + hex2(u));
    }
    return '0x' + hex2(u);
  }

  function hex2(n) { return (n & 0xFF).toString(16).toUpperCase().padStart(2, '0'); }
  function hex4(n) { return (n & 0xFFFF).toString(16).toUpperCase().padStart(4, '0'); }
  function u16le(p, i) { return ((p[i] || 0) | ((p[i + 1] || 0) << 8)) & 0xFFFF; }
  function macStr(p) {
    var s = [];
    for (var i = 0; i < 6; i++) { s.push(hex2(p[i] || 0)); }
    return s.join(':');
  }

  /* ---------------- 控制命令应答 ---------------- */
  function decodeVersion(p) {
    return { major: p[0] || 0, minor: p[1] || 0, patch: p[2] || 0,
      text: (p[0] || 0) + '.' + (p[1] || 0) + '.' + (p[2] || 0) };
  }
  function decodeStatus(p) {
    return { mode: p[0] || 0, modeName: F.modeName(p[0] || 0),
      connected: !!(p[1] || 0), profile: p[2] || 0, battery: p[3] || 0,
      sleepPolicy: p[4] || 0 };   /* 第 5 字节仅 GET_STATUS 携带；EVT_STATE(4B) 时为缺省 0 */
  }
  /* GET_MODE 应答：[mode][connected][radio]（不含档位/电量，供高频轮询） */
  function decodeMode(p) {
    return { mode: p[0] || 0, modeName: F.modeName(p[0] || 0),
      connected: !!(p[1] || 0), radio: p[2] || 0, radioName: F.radioName(p[2] || 0) };
  }
  /* GET_WIRELESS_STATE 应答：[wl_state u8]（BRIDGE_WL_*） */
  function decodeWirelessState(p) {
    var s = p[0] || 0;
    return { state: s, name: F.wlStateName(s) };
  }
  /* GET_BLE_SLOTS 应答：5B [s1][s2][s3][s4][last_active]（1=该槽已绑定；last_active 1..4，0=无） */
  function decodeBleSlots(p) {
    var slots = [!!(p[0] || 0), !!(p[1] || 0), !!(p[2] || 0), !!(p[3] || 0)];
    var bound = 0;
    for (var i = 0; i < slots.length; i++) { if (slots[i]) { bound++; } }
    return { slots: slots, lastActive: p[4] || 0, bound: bound };
  }
  function bleSlotsText(s) {
    var parts = [];
    for (var i = 0; i < s.slots.length; i++) { parts.push('槽' + (i + 1) + '=' + (s.slots[i] ? '已绑' : '空')); }
    parts.push('最近活跃=' + (s.lastActive ? ('槽' + s.lastActive) : '无'));
    return parts.join(' ');
  }
  function decodeMac(p) { return { mac: macStr(p), bytes: p }; }
  function decodeAdc(p) { return { mv: u16le(p, 0) }; }
  function decodeNvRead(p) { return { addr: p[0] || 0, data: p.slice(1) }; }
  /* GET_DIAG 应答：24B 2.4G 射频诊断（bridge_ch58x_sdk.h sdk_rf24_diag()；[6..23] 均 LE16） */
  function decodeDiag(p) {
    return {
      up: p[0] || 0, conn: p[1] || 0, bound: p[2] || 0, txBusy: p[3] || 0,
      txQCnt: p[4] || 0, hbMiss: p[5] || 0,
      reportPeriod: u16le(p, 6),
      txSubmitOk: u16le(p, 8), txSubmitFail: u16le(p, 10),
      txDoneOk: u16le(p, 12), txDoneFail: u16le(p, 14),
      txTimeout: u16le(p, 16),
      rxAny: u16le(p, 18), hopShut: u16le(p, 20), rxDataBad: u16le(p, 22)
    };
  }
  /* 诊断快照 → 单行摘要（供命令结果与监视器复用） */
  function diagText(d) {
    return 'up=' + d.up + ' conn=' + d.conn + ' bound=' + d.bound +
      ' txBusy=' + d.txBusy + ' txQ=' + d.txQCnt + ' hbMiss=' + d.hbMiss +
      ' period=' + d.reportPeriod + 'ms' +
      ' txSubmit=' + d.txSubmitOk + '/' + d.txSubmitFail +
      ' txDone=' + d.txDoneOk + '/' + d.txDoneFail + ' timeout=' + d.txTimeout +
      ' rxAny=' + d.rxAny + ' hopShut=' + d.hopShut + ' rxBad=' + d.rxDataBad;
  }
  /* HID_DIAG 应答：14B BLE HID 服务注册取证（bridge_ble_hidsvc.c；[1..8] 均 LE16） */
  function decodeHidDiag(p) {
    var reg = p[0];
    return {
      reg: (reg === undefined) ? 0xFF : reg,
      regOk: (reg !== undefined) && (reg === 0),
      hidHandle: u16le(p, 1), mapHandle: u16le(p, 3),
      rawInHandle: u16le(p, 5), rawOutHandle: u16le(p, 7),
      reportCnt: p[9] || 0, attrCnt: p[10] || 0,
      resetSrc: (p[11] || 0) & 0x07,
      stagePrev: p[12] || 0, stageNow: p[13] || 0
    };
  }
  /* HID 服务取证 → 单行摘要 */
  function hidDiagText(d) {
    return 'reg=' + (d.reg === 0xFF ? '未调用(0xFF)' : ('0x' + hex2(d.reg))) +
      ' hid=0x' + hex4(d.hidHandle) + ' map=0x' + hex4(d.mapHandle) +
      ' rawIn=0x' + hex4(d.rawInHandle) + ' rawOut=0x' + hex4(d.rawOutHandle) +
      ' reports=' + d.reportCnt + ' attrs=' + d.attrCnt +
      ' reset=' + d.resetSrc + ' stage=' + d.stagePrev + '→' + d.stageNow;
  }

  /* ---------------- 事件 ---------------- */
  function decodeState(p) { return decodeStatus(p); }   /* EVT_STATE 布局同 GET_STATUS */
  function decodeLeds(p) {
    var s = p[0] || 0;
    return { state: s, num: !!(s & 0x01), caps: !!(s & 0x02), scroll: !!(s & 0x04) };
  }
  function decodeBattery(p) { return { mv: u16le(p, 0), percent: p[2] || 0 }; }
  function decodePairResult(p) { return { result: p[0] || 0, ok: (p[0] || 0) === 0, profile: p[1] || 0 }; }
  function decodeError(p) { return { code: p[0] || 0, name: F.errCodeName(p[0] || 0) }; }
  /* EVT_HOST_POWER：[state]（0=运行/唤醒，1=挂起；bridge_types.h BRIDGE_HOST_*） */
  function decodeHostPower(p) {
    var s = p[0] || 0;
    return { state: s, awake: s === 0, name: F.hostName(s) };
  }

  /* ---------------- SET_CONFIG ---------------- */
  function decodeConfigValue(key, value) {
    var v = value || new Uint8Array(0);
    switch (key) {
      case F.CFG_KEY.REPORT_PERIOD: return { key: key, name: 'REPORT_PERIOD', value: u16le(v, 0), unit: 'ms' };
      case F.CFG_KEY.LOG_LEVEL: return { key: key, name: 'LOG_LEVEL', value: v[0] || 0 };
      case F.CFG_KEY.ADV_NAME_MAC: return { key: key, name: 'ADV_NAME_MAC', value: v[0] || 0 };
      case F.CFG_KEY.CONN_INTERVAL: return { key: key, name: 'CONN_INTERVAL', value: v[0] || 0, unit: '×1.25ms' };
      case F.CFG_KEY.BATTERY_SRC: return { key: key, name: 'BATTERY_SRC', value: v[0] || 0 };
      case F.CFG_KEY.SLEEP_POLICY: return { key: key, name: 'SLEEP_POLICY', value: v[0] || 0 };
      default: return { key: key, name: '0x' + hex2(key), value: null, hex: F.bytesToHex(v) };
    }
  }

  /* ---------------- HID 快速路径 ---------------- */
  function decodeHidKeyboard(p) {
    var mod = p[0] || 0, keys = [];
    for (var i = 2; i < 8; i++) { if (p[i]) { keys.push(keyName(p[i])); } }
    var mods = [];
    for (var b = 0; b < 8; b++) { if (mod & (1 << b)) { mods.push(keyName(0xE0 + b)); } }
    return { modifier: mod, mods: mods, keys: keys };
  }
  function decodeHidNkro(p) {
    var keys = [];
    for (var byte = 0; byte < 16; byte++) {
      var v = p[byte] || 0;
      for (var bit = 0; bit < 8; bit++) { if (v & (1 << bit)) { keys.push(keyName(byte * 8 + bit + 4)); } }
    }
    return { keys: keys };
  }
  function decodeHidConsumer(p) { return { usage: u16le(p, 0) }; }
  function decodeHidMouse(p) {
    return { buttons: p[0] || 0, dx: signed8(p[1]), dy: signed8(p[2]), wheel: signed8(p[3]) };
  }
  function decodeHidSystem(p) { return { code: p[0] || 0 }; }
  function signed8(v) { v = v || 0; return v > 127 ? v - 256 : v; }

  /* ---------------- 统一解码入口 ---------------- */
  /** 按 cmd 解码载荷；未知 cmd 返回 null */
  function decodePayload(cmd, payload) {
    var p = payload || new Uint8Array(0);
    switch (cmd) {
      case F.CMD.GET_VERSION: return decodeVersion(p);
      case F.CMD.GET_STATUS: return decodeStatus(p);
      case F.CMD.GET_MODE: return decodeMode(p);
      case F.CMD.GET_WIRELESS_STATE: return decodeWirelessState(p);
      case F.CMD.GET_BLE_SLOTS: return decodeBleSlots(p);
      case F.CMD.GET_MAC: return decodeMac(p);
      case F.CMD.GET_ADC: return decodeAdc(p);
      case F.CMD.NV_READ: return decodeNvRead(p);
      case F.CMD.GET_DIAG: return decodeDiag(p);
      case F.CMD.HID_DIAG: return decodeHidDiag(p);
      case F.EVT.STATE: return decodeState(p);
      case F.EVT.LEDS: return decodeLeds(p);
      case F.EVT.BATTERY: return decodeBattery(p);
      case F.EVT.PAIR_RESULT: return decodePairResult(p);
      case F.EVT.ERROR: return decodeError(p);
      case F.EVT.HOST_POWER: return decodeHostPower(p);
      case F.CMD.HID_KEYBOARD: return decodeHidKeyboard(p);
      case F.CMD.HID_NKRO: return decodeHidNkro(p);
      case F.CMD.HID_CONSUMER: return decodeHidConsumer(p);
      case F.CMD.HID_MOUSE: return decodeHidMouse(p);
      case F.CMD.HID_SYSTEM: return decodeHidSystem(p);
      case F.CMD.SET_CONFIG: return decodeConfigValue(p[0] || 0, p.slice(1));
      default: return null;
    }
  }

  /** 帧级别摘要字符串（供监视器/日志；容错，不抛异常） */
  function summarize(fr) {
    try {
      if (!fr) { return ''; }
      if (fr.isErr()) { return 'ERR ' + F.errCodeName(fr.errCode()); }
      if (fr.cmd === F.CMD.PING) { return fr.isAck() ? 'PONG' : 'PING'; }
      var p = fr.payload;
      switch (fr.cmd) {
        case F.CMD.GET_VERSION: return '版本 ' + decodeVersion(p).text;
        case F.CMD.GET_STATUS: case F.EVT.STATE: {
          var s = decodeStatus(p);
          var stxt = '模式=' + s.modeName + ' 连接=' + (s.connected ? '是' : '否') +
            ' 槽位=' + s.profile + ' 电量=' + s.battery + '%';
          if (p.length >= 5) { stxt += ' 休眠策略=' + (s.sleepPolicy ? 'MANUAL' : 'AUTO'); }
          return stxt;
        }
        case F.CMD.GET_MAC: return 'MAC ' + macStr(p);
        case F.CMD.GET_ADC: return 'ADC ' + u16le(p, 0) + ' mV';
        case F.CMD.GET_MODE: {
          var md = decodeMode(p);
          return '模式=' + md.modeName + ' 连接=' + (md.connected ? '是' : '否') + ' 射频=' + md.radioName;
        }
        case F.CMD.GET_WIRELESS_STATE: {
          var ws = decodeWirelessState(p);
          return '无线状态=' + ws.name;
        }
        case F.CMD.GET_BLE_SLOTS: return 'BLE槽位 ' + bleSlotsText(decodeBleSlots(p));
        case F.EVT.LEDS: {
          var l = decodeLeds(p);
          return 'LED Num=' + (l.num ? 1 : 0) + ' Caps=' + (l.caps ? 1 : 0) + ' Scroll=' + (l.scroll ? 1 : 0);
        }
        case F.EVT.BATTERY: return '电量 ' + u16le(p, 0) + ' mV / ' + (p[2] || 0) + '%';
        case F.EVT.PAIR_RESULT: {
          var r = decodePairResult(p);
          return '配对' + (r.ok ? '成功' : ('失败(' + r.result + ')')) + ' 槽位=' + r.profile;
        }
        case F.EVT.ERROR: return '错误 ' + F.errCodeName(p[0] || 0);
        case F.EVT.HOST_POWER: {
          var hp = decodeHostPower(p);
          return '主机电源 ' + (hp.awake ? '运行/唤醒' : '挂起');
        }
        case F.EVT.TUNNEL_USB: return 'USB透传 ' + p.length + 'B ' + F.bytesToHex(p);
        case F.EVT.TUNNEL_BLE: return 'BLE透传 ' + p.length + 'B ' + F.bytesToHex(p);
        case F.CMD.TUNNEL_USB: return 'USB透传 ' + p.length + 'B ' + F.bytesToHex(p);
        case F.CMD.TUNNEL_BLE: return 'BLE透传 ' + p.length + 'B ' + F.bytesToHex(p);
        case F.CMD.NV_READ: return 'NV[' + (p[0] || 0) + '] ' + F.bytesToHex(p.slice(1));
        case F.CMD.GET_DIAG: return diagText(decodeDiag(p));
        case F.CMD.HID_DIAG: return hidDiagText(decodeHidDiag(p));
        case F.CMD.SET_CONFIG: {
          var c = decodeConfigValue(p[0] || 0, p.slice(1));
          return 'CONFIG ' + c.name + '=' + (c.value === null ? c.hex : c.value) + (c.unit || '');
        }
        case F.CMD.HID_KEYBOARD: {
          var k = decodeHidKeyboard(p);
          return '键盘 ' + (k.mods.length ? k.mods.join('+') + '+' : '') + (k.keys.length ? k.keys.join(' ') : '(无键)');
        }
        case F.CMD.HID_NKRO: return 'NKRO [' + decodeHidNkro(p).keys.join(' ') + ']';
        case F.CMD.HID_CONSUMER: return '多媒体 0x' + hex2(p[0] || 0) + hex2(p[1] || 0);
        case F.CMD.HID_MOUSE: {
          var m = decodeHidMouse(p);
          return '鼠标 键=0x' + hex2(m.buttons) + ' dx=' + m.dx + ' dy=' + m.dy + ' wheel=' + m.wheel;
        }
        case F.CMD.HID_SYSTEM: return '系统键 0x' + hex2(p[0] || 0);
        case F.CMD.HID_RELEASE: return '全键释放';
        default: return fr.payload.length ? F.bytesToHex(fr.payload) : '';
      }
    } catch (e) {
      return '(解码失败: ' + e.message + ')';
    }
  }

  var api = {
    KEY_NAMES: KEY_NAMES, keyName: keyName,
    decodeVersion: decodeVersion, decodeStatus: decodeStatus, decodeMode: decodeMode,
    decodeWirelessState: decodeWirelessState,
    decodeBleSlots: decodeBleSlots, bleSlotsText: bleSlotsText,
    decodeMac: decodeMac,
    decodeAdc: decodeAdc, decodeNvRead: decodeNvRead,
    decodeDiag: decodeDiag, diagText: diagText,
    decodeHidDiag: decodeHidDiag, hidDiagText: hidDiagText,
    decodeState: decodeState, decodeLeds: decodeLeds, decodeBattery: decodeBattery,
    decodePairResult: decodePairResult, decodeError: decodeError,
    decodeHostPower: decodeHostPower,
    decodeConfigValue: decodeConfigValue,
    decodeHidKeyboard: decodeHidKeyboard, decodeHidNkro: decodeHidNkro,
    decodeHidConsumer: decodeHidConsumer, decodeHidMouse: decodeHidMouse,
    decodeHidSystem: decodeHidSystem,
    decodePayload: decodePayload, summarize: summarize
  };
  global.DbgDecode = api;
  if (typeof module === 'object' && module.exports) { module.exports = api; }
})(typeof window !== 'undefined' ? window : globalThis);
