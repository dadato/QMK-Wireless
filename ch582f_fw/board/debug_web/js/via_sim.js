/* LD7 键盘 VIA 数据模拟器 —— 网页扮演 SK32/LD7，应答桌面 VIA 经无线隧道的 VIA Raw HID
 *
 * 数据流（与 VIA 测试 Tab 的「通路 A/B」方向相反）：
 *   桌面 VIA(usevia.app)
 *     → 蓝牙(HOGP vendor RID0) 或 2.4G(Dongle RID0)
 *     → CH582F 模组
 *     → UART 桥帧 EVT_TUNNEL_BLE(0x86) / EVT_TUNNEL_USB(0x85)  [载荷 = 32B VIA Raw HID 请求]
 *     → PC COM (WebSerial，debug_web 连接)
 *     → 本模拟器：按 LD7 的 VIA 协议（镜像 quantum/via.c + dynamic_keymap + qmk_rgb_matrix）
 *         解析请求，以 LD7 的键位/宏/RGB 数据应答
 *     → 回复桥帧 CMD_TUNNEL_BLE(0x19) / CMD_TUNNEL_USB(0x18)
 *     → 模组 → 蓝牙 / 2.4G → 桌面 VIA
 *
 * 与真实固件（keyboards/sk32/ld7_oled）的对应：
 *   - VIA 协议命令枚举/语义：quantum/via.c  raw_hid_receive()  原样回 32B；
 *   - 键位/宏/NVM 布局：        quantum/dynamic_keymap.c  +  nvm (EEPROM-vendor)；
 *   - 自定义值（RGB 矩阵）：    quantum/via.c  via_qmk_rgb_matrix_get/set/save()；
 *   - 键位默认值/矩阵/RGB 默认：via.json / keymaps/default/keymap.c / config.h。
 * 本文件是纯 JS 的「LD7 VIA 固件仿真」，仅在浏览器内维护状态（可选 localStorage 持久化），
 * 不触碰任何固件。
 */
(function (global) {
  'use strict';

  var F = global.DbgFrame; /* 浏览器下已由 index.html 先行加载 */

  var REPORT_LEN = 32;

  /* ---------------- VIA 协议常量（镜像 quantum/via.h） ---------------- */
  var VIA_PROTOCOL_VERSION  = 0x000D;
  var VIA_FIRMWARE_VERSION  = 0x00000000;
  var QMK_KEYCODES_VERSION  = 0x00000008;

  var VIA_ID = {
    GET_PROTOCOL_VERSION:           0x01,
    GET_KEYBOARD_VALUE:             0x02,
    SET_KEYBOARD_VALUE:             0x03,
    DYNAMIC_KEYMAP_GET_KEYCODE:     0x04,
    DYNAMIC_KEYMAP_SET_KEYCODE:     0x05,
    DYNAMIC_KEYMAP_RESET:           0x06,
    CUSTOM_SET_VALUE:               0x07,
    CUSTOM_GET_VALUE:               0x08,
    CUSTOM_SAVE:                    0x09,
    EEPROM_RESET:                   0x0A,
    BOOTLOADER_JUMP:                0x0B,
    DYNAMIC_KEYMAP_MACRO_GET_COUNT: 0x0C,
    DYNAMIC_KEYMAP_MACRO_GET_BUFFER_SIZE: 0x0D,
    DYNAMIC_KEYMAP_MACRO_GET_BUFFER: 0x0E,
    DYNAMIC_KEYMAP_MACRO_SET_BUFFER: 0x0F,
    DYNAMIC_KEYMAP_MACRO_RESET:     0x10,
    DYNAMIC_KEYMAP_GET_LAYER_COUNT: 0x11,
    DYNAMIC_KEYMAP_GET_BUFFER:      0x12,
    DYNAMIC_KEYMAP_SET_BUFFER:      0x13,
    UNHANDLED:                      0xFF
  };

  var VIA_KBV = {
    UPTIME: 0x01, LAYOUT_OPTIONS: 0x02, SWITCH_MATRIX_STATE: 0x03,
    FIRMWARE_VERSION: 0x04, DEVICE_INDICATION: 0x05, KEYCODES_VERSION: 0x06
  };

  var VIA_CH = { CUSTOM: 0, BACKLIGHT: 1, RGBLIGHT: 2, RGB_MATRIX: 3, AUDIO: 4, LED_MATRIX: 5 };
  var VIA_RGBM = { BRIGHTNESS: 1, EFFECT: 2, EFFECT_SPEED: 3, COLOR: 4 };

  var CMD_NAME = {};
  CMD_NAME[VIA_ID.GET_PROTOCOL_VERSION] = 'GET_PROTOCOL_VERSION';
  CMD_NAME[VIA_ID.GET_KEYBOARD_VALUE] = 'GET_KEYBOARD_VALUE';
  CMD_NAME[VIA_ID.SET_KEYBOARD_VALUE] = 'SET_KEYBOARD_VALUE';
  CMD_NAME[VIA_ID.DYNAMIC_KEYMAP_GET_KEYCODE] = 'DYNAMIC_KEYMAP_GET_KEYCODE';
  CMD_NAME[VIA_ID.DYNAMIC_KEYMAP_SET_KEYCODE] = 'DYNAMIC_KEYMAP_SET_KEYCODE';
  CMD_NAME[VIA_ID.DYNAMIC_KEYMAP_RESET] = 'DYNAMIC_KEYMAP_RESET';
  CMD_NAME[VIA_ID.CUSTOM_SET_VALUE] = 'CUSTOM_SET_VALUE';
  CMD_NAME[VIA_ID.CUSTOM_GET_VALUE] = 'CUSTOM_GET_VALUE';
  CMD_NAME[VIA_ID.CUSTOM_SAVE] = 'CUSTOM_SAVE';
  CMD_NAME[VIA_ID.EEPROM_RESET] = 'EEPROM_RESET';
  CMD_NAME[VIA_ID.BOOTLOADER_JUMP] = 'BOOTLOADER_JUMP';
  CMD_NAME[VIA_ID.DYNAMIC_KEYMAP_MACRO_GET_COUNT] = 'MACRO_GET_COUNT';
  CMD_NAME[VIA_ID.DYNAMIC_KEYMAP_MACRO_GET_BUFFER_SIZE] = 'MACRO_GET_BUFFER_SIZE';
  CMD_NAME[VIA_ID.DYNAMIC_KEYMAP_MACRO_GET_BUFFER] = 'MACRO_GET_BUFFER';
  CMD_NAME[VIA_ID.DYNAMIC_KEYMAP_MACRO_SET_BUFFER] = 'MACRO_SET_BUFFER';
  CMD_NAME[VIA_ID.DYNAMIC_KEYMAP_MACRO_RESET] = 'MACRO_RESET';
  CMD_NAME[VIA_ID.DYNAMIC_KEYMAP_GET_LAYER_COUNT] = 'GET_LAYER_COUNT';
  CMD_NAME[VIA_ID.DYNAMIC_KEYMAP_GET_BUFFER] = 'KEYMAP_GET_BUFFER';
  CMD_NAME[VIA_ID.DYNAMIC_KEYMAP_SET_BUFFER] = 'KEYMAP_SET_BUFFER';
  CMD_NAME[VIA_ID.UNHANDLED] = 'UNHANDLED';

  /* ---------------- LD7 键盘定义（keyboards/sk32/ld7_oled） ---------------- */
  var LD7 = {
    name: 'LD7_OLED',
    vendorId: 0x0194,
    productId: 0x0464,
    matrixRows: 1,
    matrixCols: 7,
    layerCount: 4,                    /* DYNAMIC_KEYMAP_LAYER_COUNT 默认 4，LD7 未覆盖 */
    /* 默认层 0 键位：Z(0x1D) X(0x1B) C(0x06) V(0x19) A(0x04) LCTL(0xE0) ENT(0x28) */
    defaultKeymap: [0x1D, 0x1B, 0x06, 0x19, 0x04, 0xE0, 0x28],
    macroCount: 16,                   /* DYNAMIC_KEYMAP_MACRO_COUNT 默认 16 */
    macroSize: 1024,                  /* EEPROM-vendor（SK32 末 2KB 页）下的宏区大小 */
    rgbMaxBrightness: 180             /* RGB_MATRIX_MAXIMUM_BRIGHTNESS */
  };

  var STORAGE_KEY = 'dbg.viaSim.ld7.v1';

  /* ---------------- 工具 ---------------- */
  function be16(h, l) { return (h << 8) | l; }
  function u16be(buf, off) { return (buf[off] << 8) | buf[off + 1]; }
  function hex(u8) {
    var out = [];
    for (var i = 0; i < u8.length; i++) { out.push(('0' + u8[i].toString(16).toUpperCase()).slice(-2)); }
    return out.join(' ');
  }
  function clamp(v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }

  function nowUptime(self) {
    var t = (global.performance && global.performance.now) ? global.performance.now() : Date.now();
    var ms = t - self.t0;
    if (ms < 0) { ms = 0; }
    if (ms > 0xFFFFFFFF) { ms = 0xFFFFFFFF; }
    return ms | 0;
  }

  /* ---------------- 模拟器状态 ---------------- */

  /**
   * @param {object} opts
   *   opts.log     - (msg: string) => void
   *   opts.store   - 可选；是否持久化到 localStorage（默认 true）。Node 自检传 false。
   */
  function ViaSim(opts) {
    opts = opts || {};
    this.log = opts.log || function () {};
    this.persist = (opts.store === undefined) ? true : !!opts.store;
    this.running = false;
    this.t0 = (global.performance && global.performance.now) ? global.performance.now() : Date.now();
    this.frameCount = 0;
    this.viaCount = 0;
    this.lastSummary = null;

    this.layoutOptions = 0;
    this.rgb = {
      enabled: true,
      mode: 1,    /* RGB_MATRIX_SOLID_COLOR */
      speed: 127, /* RGB_MATRIX_DEFAULT_SPD */
      hue: 0,     /* RGB_MATRIX_DEFAULT_HUE */
      sat: 255,   /* RGB_MATRIX_DEFAULT_SAT */
      val: 180    /* RGB_MATRIX_DEFAULT_VAL */
    };
    this.keymap = new Uint8Array(LD7.layerCount * LD7.matrixRows * LD7.matrixCols * 2);
    this.macros = new Uint8Array(LD7.macroSize);
    this._resetBuffer();
    this._load();
  }

  ViaSim.prototype._resetBuffer = function () {
    this.keymap.fill(0);
    var idx = 0;
    for (var i = 0; i < LD7.matrixCols; i++) {
      var kc = (i < LD7.defaultKeymap.length) ? LD7.defaultKeymap[i] : 0;
      this.keymap[idx++] = (kc >> 8) & 0xFF;
      this.keymap[idx++] = kc & 0xFF;
    }
    this.macros.fill(0); /* 全 0 = 缓冲有效且无宏 */
  };

  /* ---------------- 持久化（可选） ---------------- */
  ViaSim.prototype._persist = function () {
    if (!this.persist) { return; }
    try {
      var obj = {
        v: 1,
        layoutOptions: this.layoutOptions >>> 0,
        rgb: this.rgb,
        keymap: Array.prototype.slice.call(this.keymap),
        macros: Array.prototype.slice.call(this.macros)
      };
      global.localStorage.setItem(STORAGE_KEY, JSON.stringify(obj));
    } catch (e) { /* 忽略（隐私模式/限域） */ }
  };

  ViaSim.prototype._load = function () {
    if (!this.persist) { return; }
    try {
      var raw = global.localStorage.getItem(STORAGE_KEY);
      if (!raw) { return; }
      var obj = JSON.parse(raw);
      if (!obj || obj.v !== 1) { return; }
      this.layoutOptions = (obj.layoutOptions >>> 0) || 0;
      if (obj.rgb) { for (var k in this.rgb) { if (obj.rgb[k] !== undefined) { this.rgb[k] = obj.rgb[k]; } } }
      if (Array.isArray(obj.keymap) && obj.keymap.length === this.keymap.length) {
        this.keymap.set(obj.keymap);
      }
      if (Array.isArray(obj.macros) && obj.macros.length === this.macros.length) {
        this.macros.set(obj.macros);
      }
    } catch (e) { /* 忽略损坏数据 */ }
  };

  /* 重置为 LD7 出厂默认（镜像 dynamic_keymap_reset + macro_reset + layout 默认） */
  ViaSim.prototype.resetDefaults = function () {
    this.layoutOptions = 0;
    this.rgb.enabled = true;
    this.rgb.mode = 1;
    this.rgb.speed = 127;
    this.rgb.hue = 0;
    this.rgb.sat = 255;
    this.rgb.val = 180;
    this._resetBuffer();
    this._persist();
  };

  /* ---------------- 动态键位读写（镜像 dynamic_keymap） ---------------- */
  ViaSim.prototype._keyOffset = function (layer, row, col) {
    return (layer * (LD7.matrixRows * LD7.matrixCols) + row * LD7.matrixCols + col) * 2;
  };
  ViaSim.prototype.getKeycode = function (layer, row, col) {
    var o = this._keyOffset(layer, row, col);
    return u16be(this.keymap, o);
  };
  ViaSim.prototype.setKeycode = function (layer, row, col, kc) {
    var o = this._keyOffset(layer, row, col);
    this.keymap[o] = (kc >> 8) & 0xFF;
    this.keymap[o + 1] = kc & 0xFF;
    this._persist();
  };

  /* ---------------- VIA 消息处理（镜像 quantum/via.c raw_hid_receive） ----------------
   * 输入/输出为同一 32B 缓冲 d（原样回发，命令/值就地修改）。
   * 返回：{ summary, detail }  供 UI/日志显示。 */
  ViaSim.prototype._process = function (d) {
    var summary = CMD_NAME[d[0]] || ('0x' + d[0].toString(16).toUpperCase());
    var detail = '';
    /* command_data = d[1..] */
    switch (d[0]) {
      case VIA_ID.GET_PROTOCOL_VERSION: {
        d[1] = (VIA_PROTOCOL_VERSION >> 8) & 0xFF;
        d[2] = VIA_PROTOCOL_VERSION & 0xFF;
        break;
      }
      case VIA_ID.GET_KEYBOARD_VALUE: {
        switch (d[1]) {
          case VIA_KBV.UPTIME: {
            var up = nowUptime(this);
            d[2] = (up >> 24) & 0xFF; d[3] = (up >> 16) & 0xFF;
            d[4] = (up >> 8) & 0xFF; d[5] = up & 0xFF;
            detail = 'uptime=' + up + 'ms';
            break;
          }
          case VIA_KBV.LAYOUT_OPTIONS: {
            var lo = this.layoutOptions >>> 0;
            d[2] = (lo >> 24) & 0xFF; d[3] = (lo >> 16) & 0xFF;
            d[4] = (lo >> 8) & 0xFF; d[5] = lo & 0xFF;
            break;
          }
          case VIA_KBV.SWITCH_MATRIX_STATE: {
            var offset = d[2];
            var rows = 28 / Math.floor((LD7.matrixCols + 7) / 8);
            var i = 3;
            for (var row = 0; row < rows && (row + offset) < LD7.matrixRows; row++) {
              /* 无物理按键按下 -> 0；如后续需要可在此模拟按键态 */
              d[i++] = 0;
            }
            break;
          }
          case VIA_KBV.FIRMWARE_VERSION: {
            var fw = VIA_FIRMWARE_VERSION >>> 0;
            d[2] = (fw >> 24) & 0xFF; d[3] = (fw >> 16) & 0xFF;
            d[4] = (fw >> 8) & 0xFF; d[5] = fw & 0xFF;
            break;
          }
          case VIA_KBV.KEYCODES_VERSION: {
            var kcv = QMK_KEYCODES_VERSION >>> 0;
            d[2] = (kcv >> 24) & 0xFF; d[3] = (kcv >> 16) & 0xFF;
            d[4] = (kcv >> 8) & 0xFF; d[5] = kcv & 0xFF;
            break;
          }
          default: {
            detail = '未知 keyboard_value=0x' + d[1].toString(16);
            d[0] = VIA_ID.UNHANDLED;
            break;
          }
        }
        break;
      }
      case VIA_ID.SET_KEYBOARD_VALUE: {
        switch (d[1]) {
          case VIA_KBV.LAYOUT_OPTIONS: {
            this.layoutOptions = ((d[2] << 24) | (d[3] << 16) | (d[4] << 8) | d[5]) >>> 0;
            this._persist();
            break;
          }
          case VIA_KBV.DEVICE_INDICATION: {
            /* 模拟器不闪灯；仅记录 */
            detail = 'device_indication=' + d[2];
            break;
          }
          default: {
            detail = '未知 keyboard_value=0x' + d[1].toString(16);
            d[0] = VIA_ID.UNHANDLED;
            break;
          }
        }
        break;
      }
      case VIA_ID.DYNAMIC_KEYMAP_GET_KEYCODE: {
        var kc = this.getKeycode(clamp(d[1], 0, LD7.layerCount - 1), clamp(d[2], 0, LD7.matrixRows - 1), clamp(d[3], 0, LD7.matrixCols - 1));
        d[4] = (kc >> 8) & 0xFF;
        d[5] = kc & 0xFF;
        detail = 'L' + d[1] + ' R' + d[2] + ' C' + d[3] + ' -> 0x' + kc.toString(16).toUpperCase().padStart(4, '0');
        break;
      }
      case VIA_ID.DYNAMIC_KEYMAP_SET_KEYCODE: {
        if (d[1] < LD7.layerCount && d[2] < LD7.matrixRows && d[3] < LD7.matrixCols) {
          var nkc = be16(d[4], d[5]);
          this.setKeycode(d[1], d[2], d[3], nkc);
          detail = 'L' + d[1] + ' R' + d[2] + ' C' + d[3] + ' = 0x' + nkc.toString(16).toUpperCase().padStart(4, '0');
        }
        break;
      }
      case VIA_ID.DYNAMIC_KEYMAP_RESET: {
        /* 重置键位到 flash 默认（仅键位缓冲） */
        var idx = 0;
        for (var c0 = 0; c0 < this.keymap.length; c0++) { this.keymap[c0] = 0; }
        for (var i0 = 0; i0 < LD7.matrixCols; i0++) {
          var dkc = (i0 < LD7.defaultKeymap.length) ? LD7.defaultKeymap[i0] : 0;
          this.keymap[idx++] = (dkc >> 8) & 0xFF;
          this.keymap[idx++] = dkc & 0xFF;
        }
        this._persist();
        break;
      }
      case VIA_ID.CUSTOM_SET_VALUE:
      case VIA_ID.CUSTOM_GET_VALUE:
      case VIA_ID.CUSTOM_SAVE: {
        /* data = [cmd, channel, value_id, value_data...] */
        this._custom(d, (d[0] === VIA_ID.CUSTOM_SET_VALUE) ? 'set' : ((d[0] === VIA_ID.CUSTOM_GET_VALUE) ? 'get' : 'save'));
        break;
      }
      case VIA_ID.EEPROM_RESET: {
        this.resetDefaults();
        break;
      }
      case VIA_ID.BOOTLOADER_JUMP: {
        /* 模拟器不能跳 bootloader，仅提示 */
        detail = 'bootloader_jump（模拟器不支持，忽略）';
        break;
      }
      case VIA_ID.DYNAMIC_KEYMAP_MACRO_GET_COUNT: {
        d[1] = LD7.macroCount;
        break;
      }
      case VIA_ID.DYNAMIC_KEYMAP_MACRO_GET_BUFFER_SIZE: {
        var ms = LD7.macroSize & 0xFFFF;
        d[1] = (ms >> 8) & 0xFF;
        d[2] = ms & 0xFF;
        break;
      }
      case VIA_ID.DYNAMIC_KEYMAP_MACRO_GET_BUFFER:
      case VIA_ID.DYNAMIC_KEYMAP_MACRO_SET_BUFFER: {
        /* [cmd][offHi][offLo][size][data...] */
        var moff = be16(d[1], d[2]);
        var msize = d[3];
        if (msize > 28) { msize = 28; }
        if (moff + msize > LD7.macroSize) { msize = LD7.macroSize - moff; }
        for (var j = 0; j < msize; j++) {
          if (d[0] === VIA_ID.DYNAMIC_KEYMAP_MACRO_GET_BUFFER) {
            d[4 + j] = this.macros[moff + j];
          } else {
            this.macros[moff + j] = d[4 + j];
          }
        }
        if (d[0] === VIA_ID.DYNAMIC_KEYMAP_MACRO_SET_BUFFER) { this._persist(); }
        break;
      }
      case VIA_ID.DYNAMIC_KEYMAP_MACRO_RESET: {
        this.macros.fill(0);
        this._persist();
        break;
      }
      case VIA_ID.DYNAMIC_KEYMAP_GET_LAYER_COUNT: {
        d[1] = LD7.layerCount;
        break;
      }
      case VIA_ID.DYNAMIC_KEYMAP_GET_BUFFER:
      case VIA_ID.DYNAMIC_KEYMAP_SET_BUFFER: {
        /* [cmd][offHi][offLo][size][data...] */
        var boff = be16(d[1], d[2]);
        var bsize = d[3];
        if (bsize > 28) { bsize = 28; }
        if (boff + bsize > this.keymap.length) { bsize = this.keymap.length - boff; }
        for (var b = 0; b < bsize; b++) {
          if (d[0] === VIA_ID.DYNAMIC_KEYMAP_GET_BUFFER) {
            d[4 + b] = this.keymap[boff + b];
          } else {
            this.keymap[boff + b] = d[4 + b];
          }
        }
        if (d[0] === VIA_ID.DYNAMIC_KEYMAP_SET_BUFFER) { this._persist(); }
        break;
      }
      default: {
        detail = '未知命令';
        d[0] = VIA_ID.UNHANDLED;
        break;
      }
    }
    return { summary: summary, detail: detail };
  };

  /* ---------------- 自定义值（qmk_rgb_matrix 通道，镜像 via_qmk_rgb_matrix_*） ---------------- */
  ViaSim.prototype._custom = function (d, op) {
    var channel = d[1];
    var vid = d[2];               /* value_id */
    var vd = d;                   /* value_data 从 d[3] 起 */
    if (channel === VIA_CH.RGB_MATRIX) {
      switch (op) {
        case 'get': {
          switch (vid) {
            case VIA_RGBM.BRIGHTNESS:
              vd[3] = Math.round(this.rgb.enabled ? ((this.rgb.val * 0xFF) / LD7.rgbMaxBrightness) : 0);
              break;
            case VIA_RGBM.EFFECT:
              vd[3] = this.rgb.enabled ? this.rgb.mode : 0;
              break;
            case VIA_RGBM.EFFECT_SPEED:
              vd[3] = this.rgb.speed;
              break;
            case VIA_RGBM.COLOR:
              vd[3] = this.rgb.hue;
              vd[4] = this.rgb.sat;
              break;
            default:
              d[0] = VIA_ID.UNHANDLED;
              break;
          }
          break;
        }
        case 'set': {
          switch (vid) {
            case VIA_RGBM.BRIGHTNESS: {
              var b = Math.round((vd[3] * LD7.rgbMaxBrightness) / 0xFF);
              this.rgb.val = clamp(b, 0, LD7.rgbMaxBrightness);
              this.rgb.enabled = this.rgb.enabled || (b > 0);
              break;
            }
            case VIA_RGBM.EFFECT: {
              if (vd[3] === 0) { this.rgb.enabled = false; }
              else { this.rgb.enabled = true; this.rgb.mode = vd[3]; }
              break;
            }
            case VIA_RGBM.EFFECT_SPEED:
              this.rgb.speed = vd[3];
              break;
            case VIA_RGBM.COLOR:
              this.rgb.hue = vd[3];
              this.rgb.sat = vd[4];
              break;
            default:
              d[0] = VIA_ID.UNHANDLED;
              break;
          }
          break;
        }
        case 'save': {
          /* 模拟器中即状态本身；持久化即可 */
          this._persist();
          break;
        }
      }
    } else {
      d[0] = VIA_ID.UNHANDLED;
    }
  };

  /* ---------------- 隧道入口 ---------------- */
  /**
   * @param {number} evtCmd  EVT_TUNNEL_BLE(0x86) 或 EVT_TUNNEL_USB(0x85)
   * @param {Uint8Array} payload  桥帧载荷（应为 32B VIA Raw HID）
   * @returns {{handled:boolean, replyCmd:number, reply:Uint8Array, summary:string, detail:string}}
   *   仅当被本模拟器处理时 handled=true。回复应答帧应经 CMD_TUNNEL_BLE/USB 发送回模组。
   */
  ViaSim.prototype.handle = function (evtCmd, payload) {
    this.frameCount++;
    var wasRunning = this.running;
    this.running = true;
    var replyCmd = (evtCmd === F.EVT.TUNNEL_BLE) ? F.CMD.TUNNEL_BLE : F.CMD.TUNNEL_USB;
    var d = new Uint8Array(REPORT_LEN);
    if (!payload || payload.length === 0) {
      return { handled: false, replyCmd: 0, reply: d, summary: '', detail: '空载荷' };
    }
    var n = Math.min(payload.length, REPORT_LEN);
    d.set(payload.subarray(0, n));
    var r = this._process(d);
    this.viaCount++;
    this.lastSummary = r.summary + (r.detail ? (' ' + r.detail) : '');
    if (!wasRunning && this.onActivity) { try { this.onActivity(); } catch (e) { /* ignore */ } }
    return { handled: true, replyCmd: replyCmd, reply: d, summary: r.summary, detail: r.detail };
  };

  /* ---------------- 快照（供 UI 显示当前状态） ---------------- */
  ViaSim.prototype.getSnapshot = function () {
    var layer0 = [];
    for (var c = 0; c < LD7.matrixCols; c++) { layer0.push(this.getKeycode(0, 0, c)); }
    return {
      frameCount: this.frameCount,
      viaCount: this.viaCount,
      lastSummary: this.lastSummary,
      layerCount: LD7.layerCount,
      layer0: layer0,
      rgb: {
        enabled: this.rgb.enabled,
        mode: this.rgb.mode,
        speed: this.rgb.speed,
        hue: this.rgb.hue,
        sat: this.rgb.sat,
        val: this.rgb.val
      }
    };
  };

  /* ---------------- 导出 ---------------- */
  var api = {
    REPORT_LEN: REPORT_LEN,
    VIA_PROTOCOL_VERSION: VIA_PROTOCOL_VERSION,
    VIA_ID: VIA_ID,
    VIA_KBV: VIA_KBV,
    VIA_CH: VIA_CH,
    VIA_RGBM: VIA_RGBM,
    CMD_NAME: CMD_NAME,
    LD7: LD7,
    ViaSim: ViaSim,
    hex: hex
  };
  global.DbgViaSim = api;
  if (typeof module === 'object' && module.exports) { module.exports = api; }
})(typeof window !== 'undefined' ? window : globalThis);
