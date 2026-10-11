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
 * HID 为复合设备、vendor 报表用 ID 5（QMK BLE 键盘固定占用 1..4）。又因两条通路
 * 现在上报相同 VID/PID，仅靠 usagePage/VID/PID/Report ID 已无法区分，故另用「是否
 * 存在键盘顶层集合」区分：Dongle IF2 为 vendor-only（无键盘集合），BLE HOGP 为单
 * 描述符复合设备（vendor 之外还有键盘 RID1/多媒体 RID2/系统 RID3）。
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

  /* 两条通路的 HID 设备现在都上报 LD7 键盘的 VID/PID，以便桌面 VIA 命中同一份键盘定义：
   *   Dongle   : VID 0x0194 / PID 0x0464（SET_USB_ID(0x20) 覆盖 USB 描述符后）
   *   BLE HOGP : VID 0x0194 / PID 0x0464（SET_USB_ID(0x20) 同时改写 SDK DIS PnP ID 后）
   * 故两条通路的过滤器完全相同，仅靠 usagePage + VID/PID 已无法区分；改为用「是否存在
   * 键盘顶层集合」区分：Dongle IF2 为 vendor-only（无键盘集合），BLE HOGP 为单描述符复合
   * 设备（vendor 之外还有键盘 RID1/多媒体 RID2/系统 RID3）。本机同时存在两个 usagePage
   * 0xFF60 的集合，选择框会同时列出二者，按 HID 路径（Dongle 复合 IF2 vs BLE HOGP
   * service 00001812）选择亦可。
   */
  var HID_VID_DONGLE = 0x0194;
  var HID_PID_DONGLE = 0x0464;
  var HID_VID_BLE    = 0x0194;
  var HID_PID_BLE    = 0x0464;

  /* Dongle IF2 Raw HID 无 Report ID；键盘模组 BLE HOGP vendor 报表为 ID 5 */
  var HID_RID_DONGLE = 0;
  var BLE_RID_RAW    = 5;

  /* 通路 A 默认筛选（Dongle） */
  var FILTER_DONGLE = [{ usagePage: HID_USAGE_PAGE, vendorId: HID_VID_DONGLE, productId: HID_PID_DONGLE }];
  /* 通路 B 筛选（键盘模组 BLE HOGP） */
  var FILTER_BLE    = [{ usagePage: HID_USAGE_PAGE, vendorId: HID_VID_BLE, productId: HID_PID_BLE }];

  /* 由外部输入的 VID/PID 动态构造 WebHID 过滤器（usagePage 固定 0xFF60）。
   * 供 app.js 读取 UI 输入后传入 ViaHid/ViaBle；参数非法返回 null，调用方回退默认。 */
  function makeFilter(vendorId, productId) {
    function ok(v) { return typeof v === 'number' && isFinite(v) && v >= 0 && v <= 0xFFFF; }
    if (!ok(vendorId) || !ok(productId)) { return null; }
    return [{ usagePage: HID_USAGE_PAGE, vendorId: vendorId, productId: productId }];
  }

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

  function nowMs() {
    return (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now();
  }

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

  /* WebHID 把每个 HID 接口暴露为独立 HIDDevice；两条通路现在 VID/PID/usagePage 全同
   * （Report ID 不同：Dongle 0 / BLE 5），仅「是否含键盘顶层集合」不同：
   *   Dongle IF2 为 vendor-only（无键盘集合）→ false
   *   BLE HOGP 单描述符复合（含键盘 Usage Page 0x01 / Usage 0x06 顶层集合）→ true
   * 据此把两通路的同 ID 集合区分开。 */
  function hasKeyboardCollection(dev) {
    var cols = (dev && dev.collections) || [];
    for (var i = 0; i < cols.length; i++) {
      var c = cols[i];
      if (c && c.usagePage === 0x01 && c.usage === 0x06) { return true; }
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

  /** 32B 自检报文（带 LE16 序号）：序号写入 byte3/byte4。
   *  quantum/via.c 的 id_get_protocol_version 分支仅改写 data[1]/data[2]，
   *  data[3..31] 原样回发，故回包 byte3/4 可还原本次序号——用于区分丢包/错序/重复。 */
  function buildSelfTestSeq(seq) {
    var buf = buildSelfTest();
    buf[3] = seq & 0xFF;
    buf[4] = (seq >> 8) & 0xFF;
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
    /* 本通路是否为「含键盘集合的复合设备」（BLE HOGP=true / Dongle IF2=false） */
    this.bleKind = !!opts.bleKind;
    /* 本通路专属的 WebHID 筛选（usagePage + VID/PID），避免选到另一条通路的集合 */
    this.filters = opts.filters || FILTER_DONGLE;
    this.device = null;
    this._onReport = null;
    this._onDisconnect = null;
    this.pending = null;
    /* 批量测试期间置 true：抑制无 waiter 时的 '←' 日志刷屏（每次回包一行会导致 10000 行） */
    this.quiet = false;
  }

  ViaHid.isSupported = function () {
    return typeof navigator !== 'undefined' && !!navigator.hid;
  };

  /** 判断已授权设备是否命中本通路的筛选条件（usagePage + VID/PID + 键盘集合判别） */
  ViaHid.prototype._matches = function (dev) {
    if (hasKeyboardCollection(dev) !== this.bleKind) { return false; }
    for (var i = 0; i < this.filters.length; i++) {
      var f = this.filters[i];
      if (f.vendorId !== undefined && dev.vendorId !== f.vendorId) { continue; }
      if (f.productId !== undefined && dev.productId !== f.productId) { continue; }
      if (f.usagePage !== undefined && !hasUsagePage(dev, f.usagePage)) { continue; }
      return true;
    }
    return false;
  };

  /** 本通路期望的设备标识（用于提示语）：VID 0x0194 / PID 0x0464 / RID（Dongle 0 / BLE 5）/ 键盘集合 */
  ViaHid.prototype._filterDesc = function () {
    var f = this.filters[0] || {};
    function h(v) { return '0x' + v.toString(16).toUpperCase().padStart(4, '0'); }
    return 'VID ' + h(f.vendorId) + ' / PID ' + h(f.productId) + ' / RID ' + this.reportId +
      (this.bleKind ? '（含键盘集合的复合设备）' : '（vendor-only 集合）');
  };

  /** 取本通路设备：优先复用本 origin 已授权的匹配设备（免弹框），否则按专属条件弹选择框 */
  ViaHid.prototype.requestDevice = async function () {
    if (!ViaHid.isSupported()) { throw new ViaError('当前浏览器不支持 WebHID'); }
    var known = await navigator.hid.getDevices();
    for (var i = 0; i < known.length; i++) {
      if (this._matches(known[i])) { return known[i]; }
    }
    var devices = await navigator.hid.requestDevice({ filters: this.filters });
    var picked = null;
    for (var j = 0; j < (devices ? devices.length : 0); j++) {
      if (hasKeyboardCollection(devices[j]) === this.bleKind) { picked = devices[j]; break; }
    }
    if (!picked) {
      throw new ViaError('未选择 ' + this._filterDesc() +
        ' 的设备（两条通路 VID/PID 相同，须按「是否含键盘集合」区分：Dongle=vendor-only / BLE=复合）');
    }
    return picked;
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

  /** 复合设备里键盘等其它报表也会走 inputreport，故按本通路约定的 reportId（Dongle=0 /
   *  键盘模组 BLE=5）且长度 32B 过滤出 VIA Raw HID 报文。BLE 复合描述符里键盘 RID1/
   *  多媒体 RID2/系统 RID3 的输入报表 reportId≠0，会被此处滤除。
   *  批量测试时 waiter 可带 match(u8) 谓词：仅谓词命中的回包 resolve，
   *  未命中的回包视作 stale（错序/重复）收集到 waiter.stale，不打断本次等待。 */
  ViaHid.prototype._handleReport = function (e) {
    if (e.reportId !== this.reportId || e.data.byteLength !== REPORT_LEN) { return; }
    var u8 = new Uint8Array(e.data.buffer, e.data.byteOffset, e.data.byteLength);
    var waiter = this.pending;
    if (!waiter) { if (!this.quiet) { this.log('INFO', '← ' + hex(u8)); } return; }
    if (waiter.match && !waiter.match(u8)) {
      if (waiter.stale) { waiter.stale.push(u8); }
      return;
    }
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

  /** 严格单发（无重试）：写一次并等待 match(u8) 命中的回包；超时即丢包。
   *  返 { rx, stale }：stale 为等待期间收到的未命中回包（错序/重复）。供 soak 统计用。 */
  ViaHid.prototype.requestOnce = async function (buf, timeoutMs, matchFn) {
    if (!this.isConnected()) { throw new ViaError('HID 设备未连接'); }
    timeoutMs = timeoutMs || DEFAULT_TIMEOUT_MS;
    var self = this;
    var stale = [];
    var p = new Promise(function (resolve, reject) {
      var timer = setTimeout(function () {
        if (self.pending && self.pending.timer === timer) { self.pending = null; }
        reject(new ViaTimeout('等待回包超时（' + timeoutMs + 'ms）'));
      }, timeoutMs);
      self.pending = { resolve: resolve, reject: reject, timer: timer, match: matchFn || null, stale: stale };
    });
    try {
      await this.device.sendReport(this.reportId, buf);
    } catch (e) {
      this._dropPending(p);
      throw e;
    }
    var rx = await p;
    return { rx: rx, stale: stale };
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
   * 故通路 B 与通路 A 同为 WebHID，Report ID 不同（0 vs 5）；又因两条通路上报相同
   * VID/PID，另以「是否含键盘顶层集合」区分（BLE=true / Dongle=false）。 */

  function ViaBle(opts) {
    opts = opts || {};
    ViaHid.call(this, {
      log: opts.log,
      onDisconnect: opts.onDisconnect,
      reportId: BLE_RID_RAW,
      bleKind: true,
      filters: opts.filters || FILTER_BLE
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

  /* ---------------- 批量稳定性测试（soak） ----------------
   * 与命令行 soak 同口径：单发（无重试）→ 超时即丢包；按序号匹配回包 →
   * 非匹配回包计 stale（错序/重复）。统计成功/丢包率 + RTT min/mean/median/P95(/P99)。 */

  /** 升序数组的分位值（p∈[0,1]，最近秩法） */
  function percentile(sorted, p) {
    if (!sorted.length) { return null; }
    var idx = Math.ceil(p * sorted.length) - 1;
    if (idx < 0) { idx = 0; }
    if (idx >= sorted.length) { idx = sorted.length - 1; }
    return sorted[idx];
  }

  /** 统计一组样本：{ n, min, max, mean, median, p95, p99 } */
  function computeStats(arr) {
    var n = arr.length;
    if (!n) { return { n: 0, min: null, max: null, mean: null, median: null, p95: null, p99: null }; }
    var s = arr.slice().sort(function (a, b) { return a - b; });
    var sum = 0;
    for (var i = 0; i < n; i++) { sum += s[i]; }
    return {
      n: n,
      min: s[0],
      max: s[n - 1],
      mean: sum / n,
      median: percentile(s, 0.5),
      p95: percentile(s, 0.95),
      p99: percentile(s, 0.99)
    };
  }

  /** 批量稳定性测试。
   *  opts: count(默认 10000) / gapMs(默认 1) / timeoutMs(默认 800) /
   *        onProgress({done,total,ok,loss,bad,stale}) / isCancelled()
   *  返回: { total, done, ok, loss, bad, stale, lossRate, rtt, elapsedMs, cancelled }
   *  说明: ok=成功回包数；loss=超时/写失败数；bad=回包非法（id/ver 不符）数；
   *        stale=错序或重复的未命中回包数。 */
  async function soak(transport, opts) {
    opts = opts || {};
    var total = opts.count || 10000;
    var gapMs = (opts.gapMs === undefined) ? 1 : opts.gapMs;
    var timeoutMs = opts.timeoutMs || 800;
    var onProgress = opts.onProgress || function () {};
    var isCancelled = opts.isCancelled || function () { return false; };

    var prevQuiet = transport.quiet;
    transport.quiet = true;   /* 抑制逐包 '←' 日志 */
    var rtts = [];
    var loss = 0, bad = 0, stale = 0, done = 0, cancelled = false;
    var t0 = nowMs();
    try {
      for (var i = 0; i < total; i++) {
        if (isCancelled()) { cancelled = true; break; }
        var seq = i & 0xFFFF;
        var tx = buildSelfTestSeq(seq);
        var ts = nowMs();
        try {
          var r = await transport.requestOnce(tx, timeoutMs, function (u8) {
            return u8[3] === (seq & 0xFF) && u8[4] === ((seq >> 8) & 0xFF);
          });
          stale += r.stale.length;
          var v = checkReply(r.rx);
          if (v.ok) { rtts.push(nowMs() - ts); } else { bad++; }
        } catch (e) {
          loss++;
        }
        done++;
        onProgress({ done: done, total: total, ok: rtts.length, loss: loss, bad: bad, stale: stale });
        if (gapMs > 0) { await sleep(gapMs); }
      }
    } finally {
      transport.quiet = prevQuiet;
    }
    return {
      total: total,
      done: done,
      ok: rtts.length,
      loss: loss,
      bad: bad,
      stale: stale,
      lossRate: done ? (loss / done) : 0,
      rtt: computeStats(rtts),
      elapsedMs: nowMs() - t0,
      cancelled: cancelled
    };
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
    makeFilter: makeFilter,
    ViaHid: ViaHid,
    ViaBle: ViaBle,
    ViaError: ViaError,
    ViaTimeout: ViaTimeout,
    buildSelfTest: buildSelfTest,
    buildSelfTestSeq: buildSelfTestSeq,
    checkReply: checkReply,
    buildOtaGetInfo: buildOtaGetInfo,
    parseOtaGetInfo: parseOtaGetInfo,
    roundTrip: roundTrip,
    soak: soak,
    computeStats: computeStats,
    percentile: percentile,
    hex: hex
  };
})(typeof window !== 'undefined' ? window : this);
