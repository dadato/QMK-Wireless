/* VIA 隧道回路测试 —— 网页 → 蓝牙/2.4G → 模组 → UART → SK32 → 回程
 *
 * 两条通路均为 WebHID（usagePage 0xFF60 / usage 0x61 的 QMK Raw HID 集合），
 * 32B 裸透传：无 Report ID 前缀、无 status 字节，与 ota_web 的 OTA 报文语义不同。
 *   A. WebHID → Dongle IF2 EP3（Report ID 0）→ 2.4G → 键盘模组
 *              → UART EVT_TUNNEL_USB(0x85) → SK32 raw_hid_receive
 *   B. WebHID → 键盘模组 BLE HOGP vendor 集合（Report ID 5，32B IN/OUT）
 *              → UART EVT_TUNNEL_BLE(0x86) → SK32 raw_hid_receive
 *
 * 两条通路的 Report ID 语义不同：Dongle 侧 Raw HID 无编号（ID 0），键盘模组 BLE
 * HID 为复合设备、vendor 报表用 ID 5（QMK BLE 键盘固定占用 1..4）。
 *
 * 自检报文：VIA id_get_protocol_version(0x01)。SK32 的 quantum/via.c 原样回发 32B，
 * 其中 data[1]=VIA_PROTOCOL_VERSION>>8=0x00、data[2]=VIA_PROTOCOL_VERSION&0xFF=0x0D。
 * 首字节取 0x01，天然避开 Dongle 对首 3 字节 'O''T''A'(0x4F 0x54 0x41) 的 OTA 拦截。
 */
(function (global) {
  'use strict';

  var REPORT_LEN = 32;

  /* 两条通路共用的 QMK Raw HID 集合（Usage Page 0xFF60 / Usage 0x61） */
  var HID_USAGE_PAGE = 0xFF60;

  /* 除 Report ID 不同外，两条通路的 HID 设备 VID/PID 也不同。本机同时存在两个
   * usagePage 0xFF60 的集合，若只用 usagePage 过滤，选择框会同时列出 Dongle 与键盘
   * 模组 BLE 两个设备、极易选错（选错后写/读静默失败，表现为连接成功却超时）。
   *   Dongle   : VID 0x0194 / PID 0x0463（core/bridge_config.h BRIDGE_USB_VID/PID）
   *   BLE HOGP : VID 0x07D7 / PID 0x0000（SDK devinfoservice.c devInfoPnpId 默认值）
   */
  var HID_VID_DONGLE = 0x0194;
  var HID_PID_DONGLE = 0x0463;
  var HID_VID_BLE    = 0x07D7;
  var HID_PID_BLE    = 0x0000;

  /* Dongle IF2 Raw HID 无 Report ID；键盘模组 BLE HOGP vendor 报表为 ID 5 */
  var HID_RID_DONGLE = 0;
  var BLE_RID_RAW    = 5;

  /* 通路 A 默认筛选（Dongle） */
  var FILTER_DONGLE = [{ usagePage: HID_USAGE_PAGE, vendorId: HID_VID_DONGLE, productId: HID_PID_DONGLE }];
  /* 通路 B 筛选（键盘模组 BLE HOGP） */
  var FILTER_BLE    = [{ usagePage: HID_USAGE_PAGE, vendorId: HID_VID_BLE, productId: HID_PID_BLE }];

  var VIA_ID_GET_PROTOCOL_VERSION = 0x01;
  var VIA_PROTOCOL_VERSION        = 0x000D;

  /* 默认超时/重试：2.4G 上行回程有延迟且易失——单次 OUT 写（即便超时拉到 8s）取不到回包，
   * 需后续 OUT 写触发/命中上行。故默认对齐 tools/ble_via_probe.py 的「≥2 次写、每次 ≥3s 窗口」：
   * 超时 3000ms、重试 2（合计最多 3 次写）。BLE 通路首写即回（~50ms），提高默认值对其无副作用。 */
  var DEFAULT_TIMEOUT_MS = 3000;
  var DEFAULT_RETRIES    = 2;

  function ViaError(msg, code) {
    this.name = 'ViaError';
    this.message = msg;
    this.code = (code === undefined) ? null : code;
  }
  ViaError.prototype = Object.create(Error.prototype);
  ViaError.prototype.constructor = ViaError;

  function ViaTimeout(msg) {
    ViaError.call(this, msg);
    this.name = 'ViaTimeout';
  }
  ViaTimeout.prototype = Object.create(ViaError.prototype);
  ViaTimeout.prototype.constructor = ViaTimeout;

  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  function hex(u8) {
    var out = [];
    for (var i = 0; i < u8.length; i++) {
      out.push(('0' + u8[i].toString(16).toUpperCase()).slice(-2));
    }
    return out.join(' ');
  }

  /* WebHID 只暴露顶层集合，vendor 集合可能嵌在子集合里，故需递归查找 usagePage */
  function colHasPage(col, page) {
    if (!col) { return false; }
    if (col.usagePage === page) { return true; }
    var kids = col.children || [];
    for (var i = 0; i < kids.length; i++) {
      if (colHasPage(kids[i], page)) { return true; }
    }
    return false;
  }

  function hasUsagePage(dev, page) {
    var cols = (dev && dev.collections) || [];
    for (var i = 0; i < cols.length; i++) {
      if (colHasPage(cols[i], page)) { return true; }
    }
    return false;
  }

  /* ---------------- 自检报文 / 判定 ---------------- */

  /** 32B 自检报文：byte0 = id_get_protocol_version，byte1/byte2 = VIA 协议版本（0x000D） */
  function buildSelfTest() {
    var buf = new Uint8Array(REPORT_LEN);
    buf[0] = VIA_ID_GET_PROTOCOL_VERSION;
    buf[1] = (VIA_PROTOCOL_VERSION >> 8) & 0xFF;   /* 0x00 */
    buf[2] = VIA_PROTOCOL_VERSION & 0xFF;          /* 0x0D */
    return buf;
  }

  /** 判定 SK32 的 VIA 回包：byte0=0x01、byte1=0x00、byte2=0x0D */
  function checkReply(u8) {
    if (!u8 || u8.length < 3) {
      return { ok: false, detail: '回包长度不足（' + (u8 ? u8.length : 0) + 'B）' };
    }
    var hi = (VIA_PROTOCOL_VERSION >> 8) & 0xFF;
    var lo = VIA_PROTOCOL_VERSION & 0xFF;
    var ok = (u8[0] === VIA_ID_GET_PROTOCOL_VERSION) && (u8[1] === hi) && (u8[2] === lo);
    return {
      ok: ok,
      detail: (ok ? '回包合法' : ('回包不符：id=0x' + u8[0].toString(16).toUpperCase() +
        ' ver=0x' + ((u8[1] << 8 | u8[2]).toString(16).toUpperCase().padStart(4, '0')))) +
        '（期望 id=01 ver=0x000D）'
    };
  }

  /* ---------------- Dongle 本地诊断（HID OTA GET_INFO） ----------------
   * Dongle 无独立日志出口，其射频层建链取证计数经本地 HID OTA 通道读出：
   * 向 EP3 OUT 发 'O''T''A'(0x4F 0x54 0x41) + sub(0x06) + 补零，共 32B。
   * Dongle 在 App 内应答 32B：[status][mode][fw_rev LE16][vid LE16][pid LE16][diag 24B]。
   * 该通道不经 2.4G，故即便回路超时也能判定射频链路状态。 */

  var OTA_SUB_GET_INFO = 0x06;

  /** 32B GET_INFO 请求：'OTA' + sub(0x06) + 补零 */
  function buildOtaGetInfo() {
    var buf = new Uint8Array(REPORT_LEN);
    buf[0] = 0x4F; buf[1] = 0x54; buf[2] = 0x41; buf[3] = OTA_SUB_GET_INFO;
    return buf;
  }

  function le16(u8, off) { return (u8[off] | (u8[off + 1] << 8)) & 0xFFFF; }

  /** 解析 GET_INFO 应答（32B）：diag = [up][conn][bound][txBusy][txQ][hbMiss][period LE16][8×LE16 计数] */
  function parseOtaGetInfo(u8) {
    if (!u8 || u8.length < 9) {
      return { ok: false, detail: '应答长度不足（' + (u8 ? u8.length : 0) + 'B）' };
    }
    if (u8[0] !== 0) {
      return { ok: false, status: u8[0], detail: 'status=0x' + u8[0].toString(16).toUpperCase() + '（非 OK）' };
    }
    var d = u8.slice(8, 32);   /* 24B 诊断快照 */
    return {
      ok: true,
      mode: u8[1],
      fwRev: le16(u8, 2),
      vid: le16(u8, 4),
      pid: le16(u8, 6),
      up: d[0], conn: d[1], bound: d[2],
      txBusy: d[3], txQ: d[4], hbMiss: d[5],
      periodMs: d[6] | (d[7] << 8),
      counters: {
        txSubmitOk: le16(d, 8),
        txSubmitFail: le16(d, 10),
        txDoneOk: le16(d, 12),
        txDoneFail: le16(d, 14),
        txTimeout: le16(d, 16),
        rxAny: le16(d, 18),
        hopShut: le16(d, 20),
        rxDataBad: le16(d, 22)
      }
    };
  }

  /* ---------------- 通路 A：WebHID → Dongle → 2.4G ---------------- */

  function ViaHid(opts) {
    opts = opts || {};
    this.log = opts.log || function () {};
    this.onDisconnect = opts.onDisconnect || function () {};
    /* Raw HID 报表编号：Dongle=0（无编号），键盘模组 BLE=5（复合 HID vendor 报表） */
    this.reportId = (opts.reportId === undefined) ? HID_RID_DONGLE : opts.reportId;
    /* 本通路专属的 WebHID 筛选（usagePage + VID/PID），避免选到另一条通路的集合 */
    this.filters = opts.filters || FILTER_DONGLE;
    this.device = null;
    this._onReport = null;
    this._onDisconnect = null;
    this.pending = null;
  }

  ViaHid.isSupported = function () {
    return typeof navigator !== 'undefined' && !!navigator.hid;
  };

  /** 判断已授权设备是否命中本通路的筛选条件 */
  ViaHid.prototype._matches = function (dev) {
    for (var i = 0; i < this.filters.length; i++) {
      var f = this.filters[i];
      if (f.vendorId !== undefined && dev.vendorId !== f.vendorId) { continue; }
      if (f.productId !== undefined && dev.productId !== f.productId) { continue; }
      if (f.usagePage !== undefined && !hasUsagePage(dev, f.usagePage)) { continue; }
      return true;
    }
    return false;
  };

  /** 本通路期望的设备标识（用于提示语）：VID 0x0194 / PID 0x0463 */
  ViaHid.prototype._filterDesc = function () {
    var f = this.filters[0] || {};
    function h(v) { return '0x' + v.toString(16).toUpperCase().padStart(4, '0'); }
    return 'VID ' + h(f.vendorId) + ' / PID ' + h(f.productId);
  };

  /** 取本通路设备：优先复用本 origin 已授权的匹配设备（免弹框），否则按专属条件弹选择框 */
  ViaHid.prototype.requestDevice = async function () {
    if (!ViaHid.isSupported()) { throw new ViaError('当前浏览器不支持 WebHID'); }
    var known = await navigator.hid.getDevices();
    for (var i = 0; i < known.length; i++) {
      if (this._matches(known[i])) { return known[i]; }
    }
    var devices = await navigator.hid.requestDevice({ filters: this.filters });
    if (!devices || !devices.length) {
      throw new ViaError('未选择设备（本通路应选 ' + this._filterDesc() + ' 的 usagePage 0xFF60 集合）');
    }
    return devices[0];
  };

  ViaHid.prototype.open = async function (dev) {
    if (!dev.opened) { await dev.open(); }
    var self = this;
    this._detach();
    this.device = dev;
    this._onReport = function (e) { self._handleReport(e); };
    this._onDisconnect = function () {
      self.log('WARN', 'Dongle HID 设备已断开');
      self.onDisconnect();
    };
    dev.addEventListener('inputreport', this._onReport);
    dev.addEventListener('disconnect', this._onDisconnect);
    return dev;
  };

  ViaHid.prototype._detach = function () {
    if (this.device) {
      try {
        if (this._onReport) { this.device.removeEventListener('inputreport', this._onReport); }
        if (this._onDisconnect) { this.device.removeEventListener('disconnect', this._onDisconnect); }
      } catch (e) { /* ignore */ }
    }
    this.device = null;
    this._onReport = null;
    this._onDisconnect = null;
  };

  ViaHid.prototype.isConnected = function () { return !!this.device && this.device.opened; };

  ViaHid.prototype.close = async function () {
    var dev = this.device;
    this._detach();
    this._dropPending(null);
    if (dev && dev.opened) { try { await dev.close(); } catch (e) { /* ignore */ } }
  };

  /** 复合设备里键盘等其它报表也会走 inputreport，故按本通路约定的 reportId
   *  且长度 32B 过滤出 VIA Raw HID 报文（Dongle=0 / 键盘模组 BLE=5）。 */
  ViaHid.prototype._handleReport = function (e) {
    if (e.reportId !== this.reportId || e.data.byteLength !== REPORT_LEN) { return; }
    var u8 = new Uint8Array(e.data.buffer, e.data.byteOffset, e.data.byteLength);
    var waiter = this.pending;
    if (!waiter) { this.log('INFO', '← ' + hex(u8)); return; }
    this.pending = null;
    clearTimeout(waiter.timer);
    waiter.resolve(u8);
  };

  ViaHid.prototype._waitReply = function (timeoutMs) {
    var self = this;
    return new Promise(function (resolve, reject) {
      var timer = setTimeout(function () {
        if (self.pending && self.pending.timer === timer) { self.pending = null; }
        reject(new ViaTimeout('等待 EP3 IN 回包超时（' + timeoutMs + 'ms）'));
      }, timeoutMs);
      self.pending = { resolve: resolve, reject: reject, timer: timer };
    });
  };

  /** 放弃当前待答：清定时器、置空 pending 并吞掉未处理拒绝（重试前必调） */
  ViaHid.prototype._dropPending = function (p) {
    if (this.pending) { clearTimeout(this.pending.timer); this.pending = null; }
    if (p && p.catch) { p.catch(function () {}); }
  };

  ViaHid.prototype.request = async function (buf, timeoutMs, retries) {
    if (!this.isConnected()) { throw new ViaError('HID 设备未连接'); }
    timeoutMs = timeoutMs || DEFAULT_TIMEOUT_MS;
    retries = (retries === undefined) ? DEFAULT_RETRIES : retries;
    var lastErr = null;
    for (var attempt = 0; attempt <= retries; attempt++) {
      var p = this._waitReply(timeoutMs);
      var wrote = true;
      try {
        this.log('INFO', '→ ' + hex(buf));
        await this.device.sendReport(this.reportId, buf);
      } catch (e) {
        wrote = false;
        this._dropPending(p);
        lastErr = e;
        this.log('WARN', '写 Raw HID（RID ' + this.reportId + '）失败（第 ' + (attempt + 1) + '/' + (retries + 1) + ' 次）：' + e.message);
      }
      if (wrote) {
        try {
          return await p;
        } catch (e) {
          lastErr = e;
          this.log('WARN', '第 ' + (attempt + 1) + '/' + (retries + 1) + ' 次无回包');
        }
      }
      if (attempt < retries) { await sleep(120); }
    }
    throw lastErr || new ViaTimeout('无回包');
  };

  /** 读取 Dongle 本地 2.4G 诊断（不经 2.4G，仅走 WebHID ↔ Dongle） */
  ViaHid.prototype.otaGetInfo = async function (timeoutMs, retries) {
    var rx = await this.request(buildOtaGetInfo(), timeoutMs, retries);
    return { rx: rx, parsed: parseOtaGetInfo(rx) };
  };

  /* ---------------- 通路 B：WebHID → 键盘模组 BLE（HOGP vendor RID 5） ----------------
   * 键盘模组对 BLE 主机提供 HOGP：键盘/多媒体/系统/LED 之外，另有一组 QMK Raw HID
   * vendor 集合（Usage Page 0xFF60 / Usage 0x61，Report ID 5，32B IN+OUT）。其 OUT
   * 写入被模组转发为 UART EVT_TUNNEL_BLE(0x86) → SK32 raw_hid_receive；SK32 的
   * raw_hid_send 经 br_raw() 走 CMD_TUNNEL_BLE(0x19) 回模组，由 vendor IN notify 送出。
   * 故通路 B 与通路 A 同为 WebHID，仅 Report ID 不同（0 vs 5）。 */

  function ViaBle(opts) {
    opts = opts || {};
    ViaHid.call(this, {
      log: opts.log,
      onDisconnect: opts.onDisconnect,
      reportId: BLE_RID_RAW,
      filters: FILTER_BLE
    });
  }
  ViaBle.prototype = Object.create(ViaHid.prototype);
  ViaBle.prototype.constructor = ViaBle;

  ViaBle.isSupported = function () { return ViaHid.isSupported(); };

  /* ---------------- 往返自检 ---------------- */

  /** 发 32B 自检报文并校验回包；transport 需实现 request(buf, timeoutMs, retries) */
  async function roundTrip(transport, opts) {
    opts = opts || {};
    var timeoutMs = opts.timeoutMs || DEFAULT_TIMEOUT_MS;
    var retries = (opts.retries === undefined) ? DEFAULT_RETRIES : opts.retries;
    var tx = buildSelfTest();
    var t0 = Date.now();
    var rx = await transport.request(tx, timeoutMs, retries);
    var elapsed = Date.now() - t0;
    var verdict = checkReply(rx);
    return { ok: verdict.ok, tx: tx, rx: rx, elapsedMs: elapsed, detail: verdict.detail };
  }

  global.DbgVia = {
    REPORT_LEN: REPORT_LEN,
    VIA_PROTOCOL_VERSION: VIA_PROTOCOL_VERSION,
    HID_VID_DONGLE: HID_VID_DONGLE,
    HID_PID_DONGLE: HID_PID_DONGLE,
    HID_VID_BLE: HID_VID_BLE,
    HID_PID_BLE: HID_PID_BLE,
    FILTER_DONGLE: FILTER_DONGLE,
    FILTER_BLE: FILTER_BLE,
    ViaHid: ViaHid,
    ViaBle: ViaBle,
    ViaError: ViaError,
    ViaTimeout: ViaTimeout,
    buildSelfTest: buildSelfTest,
    checkReply: checkReply,
    buildOtaGetInfo: buildOtaGetInfo,
    parseOtaGetInfo: parseOtaGetInfo,
    roundTrip: roundTrip,
    hex: hex
  };
})(typeof window !== 'undefined' ? window : this);
