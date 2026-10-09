/* 通路 A：接收器 Dongle —— WebHID（EP3，32B，reportId=0，usagePage 0xFF60）
 *
 * 协议（SPEC §5）：OUT 报告 byte0..2='O','T','A'，byte3=SUB，byte4..31=ARGS；
 *   IN 报告 byte0=status（0=OK），byte1..31=data。
 * SUB：0x01 ENTER_OTA / 0x02 FW_BEGIN / 0x03 FW_DATA(≤24B) / 0x04 FW_END / 0x05 FW_ABORT / 0x06 GET_INFO
 * 方案A：收包全程留在 App（ENTER_OTA 不复位、不应答），FW_END 校验通过后 App 写 IAP 标志并复位，
 * 由常驻 BOOT 把 Stage 搬到 APP_A；全程 USB 无断连（无中途重枚举）。waitForMode 为可选兜底。
 */
(function (global) {
  'use strict';

  var F = global.OtaFrame;
  var OtaError = F.OtaError;
  var OtaTimeout = F.OtaTimeout;

  var USAGE_PAGE = 0xFF60;
  var REPORT_LEN = 32;
  var ARGS_LEN = 28;        /* 32 - 3(magic) - 1(sub) */
  var MAX_IMAGE = 216 * 1024;
  var CHUNK = 24;           /* FW_DATA 有效数据 ≤24B（4 对齐） */

  var MAGIC = [0x4F, 0x54, 0x41]; /* 'O','T','A' */

  var SUB = {
    ENTER_OTA: 0x01,
    FW_BEGIN: 0x02,
    FW_DATA: 0x03,
    FW_END: 0x04,
    FW_ABORT: 0x05,
    GET_INFO: 0x06
  };

  var MODE_APP = 0;
  var MODE_IAP = 1;

  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  function HidOta(opts) {
    opts = opts || {};
    this.log = opts.log || function () {};
    this.onProgress = opts.onProgress || function () {};
    this.onDisconnect = opts.onDisconnect || function () {};
    this.device = null;
    this._onReport = null;
    this._onDisconnect = null;
    this.pending = null;
    this.abortRequested = false;
  }

  HidOta.isSupported = function () {
    return typeof navigator !== 'undefined' && 'hid' in navigator;
  };

  /* ---------------- 设备发现 / 连接 ---------------- */

  HidOta.prototype._isOtaIface = function (dev) {
    if (!dev || !dev.collections) { return false; }
    for (var i = 0; i < dev.collections.length; i++) {
      if (dev.collections[i].usagePage === USAGE_PAGE) { return true; }
    }
    return false;
  };

  HidOta.prototype._attach = function (dev) {
    var self = this;
    this._detach();
    this.device = dev;
    this._onReport = function (e) { self._handleReport(e); };
    this._onDisconnect = function () {
      self.log('WARN', '设备已断开（复位/枚举中）');
      self.onDisconnect();
    };
    dev.addEventListener('inputreport', this._onReport);
    dev.addEventListener('disconnect', this._onDisconnect);
  };

  HidOta.prototype._detach = function () {
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

  /** 弹选择框取设备（首次需用户手势）；用 usagePage 0xFF60 过滤 VIA Raw HID 接口 */
  HidOta.prototype.requestDevice = async function () {
    if (!HidOta.isSupported()) { throw new OtaError('当前浏览器不支持 WebHID'); }
    var devices = await navigator.hid.requestDevice({ filters: [{ usagePage: USAGE_PAGE }] });
    if (!devices || !devices.length) { throw new OtaError('未选择设备'); }
    return devices[0];
  };

  /** 打开并挂监听（打开后校验 GET_INFO.mode） */
  HidOta.prototype.open = async function (dev, expectedMode) {
    if (!dev.opened) { await dev.open(); }
    this._attach(dev);
    var info = null;
    try {
      /* 刚重枚举/刚打开时首包 IN 常丢失，给 1 次重试（retries=1）避免误判未响应 */
      info = await this.getInfo(1000, 1);
    } catch (e) {
      this.log('WARN', 'GET_INFO 未响应（固件可能尚未实现该子命令）：' + e.message);
    }
    if (info && expectedMode !== undefined && expectedMode !== null && info.mode !== expectedMode) {
      await this.close();
      throw new OtaError('设备处于 ' + modeName(info.mode) + '，期望 ' + modeName(expectedMode));
    }
    return info;
  };

  HidOta.prototype.isConnected = function () { return !!this.device && this.device.opened; };

  HidOta.prototype.close = async function () {
    var dev = this.device;
    this._detach();
    if (dev && dev.opened) {
      try { await dev.close(); } catch (e) { /* ignore */ }
    }
  };

  /* ---------------- 报文收发 ---------------- */

  HidOta.prototype._send = async function (sub, args) {
    if (!this.device || !this.device.opened) { throw new OtaError('HID 设备未连接'); }
    var buf = new Uint8Array(REPORT_LEN);
    buf[0] = MAGIC[0]; buf[1] = MAGIC[1]; buf[2] = MAGIC[2];
    buf[3] = sub;
    if (args && args.length) { buf.set(args.subarray(0, ARGS_LEN), 4); }
    await this.device.sendReport(0, buf);
    this.log('INFO', '→ SUB=0x' + sub.toString(16).toUpperCase().padStart(2, '0') + ' ' + hex(args));
  };

  HidOta.prototype._waitReply = function (sub, timeoutMs) {
    var self = this;
    return new Promise(function (resolve, reject) {
      self.pending = {
        sub: sub,
        resolve: resolve,
        reject: reject,
        timer: setTimeout(function () {
          if (self.pending && self.pending.sub === sub) { self.pending = null; }
          reject(new OtaTimeout('等待 SUB=0x' + sub.toString(16).toUpperCase() + ' 应答超时 (' + timeoutMs + 'ms)'));
        }, timeoutMs)
      };
    });
  };

  /** 放弃当前待答：清掉其超时定时器、置空 pending 并吞掉未处理拒绝。
   *  重试前必须先调用：否则上一轮的陈旧定时器会在重试等待期间触发，
   *  按 sub 相同误清新 pending，把重试的应答当作「无 waiter」丢弃。 */
  HidOta.prototype._dropPending = function (p) {
    if (this.pending) { clearTimeout(this.pending.timer); this.pending = null; }
    if (p && p.catch) { p.catch(function () {}); }
  };

  HidOta.prototype._handleReport = function (e) {
    if (e.reportId !== 0) { return; }
    var u8 = new Uint8Array(e.data.buffer, e.data.byteOffset, e.data.byteLength);
    var status = u8.length ? u8[0] : 0;
    var data = u8.subarray(1);
    var waiter = this.pending;
    if (!waiter) {
      this.log('INFO', '← status=' + F.bridgeErrName(status) + ' data=' + hex(data));
      return;
    }
    this.pending = null;
    clearTimeout(waiter.timer);
    if (status !== 0) {
      waiter.reject(new OtaError('设备返回错误：' + F.bridgeErrName(status), status));
    } else {
      waiter.resolve({ status: status, data: data });
    }
  };

  /** 发子命令并等 EP3 IN 应答 */
  HidOta.prototype.request = async function (sub, args, timeoutMs, retries) {
    timeoutMs = timeoutMs || 1200;
    retries = (retries === undefined) ? 1 : retries;
    var lastErr = null;
    var tag = 'SUB=0x' + sub.toString(16).toUpperCase();
    for (var attempt = 0; attempt <= retries; attempt++) {
      var p = this._waitReply(sub, timeoutMs);
      var wrote = true;
      try {
        await this._send(sub, args);
      } catch (e) {
        /* 瞬时写失败（USB/HID 抖动，并非断开）：按一次「无应答」重试同一帧。
         * 设备对重复帧幂等确认（见 core/bridge_ota.c 的 bridge_ota_data），重发安全。
         * 否则传输途中任意一次 sendReport 抖动都会中断整轮升级。 */
        wrote = false;
        this._dropPending(p);
        lastErr = e;
        this.log('WARN', tag + ' 写失败（第 ' + (attempt + 1) + '/' + (retries + 1) + ' 次）：' + e.message);
      }
      if (wrote) {
        try {
          var r = await p;
          this.log('OK', '← status=OK data=' + hex(r.data));
          return r;
        } catch (e) {
          lastErr = e;
          if (e instanceof OtaError && !(e instanceof OtaTimeout)) { throw e; }
          this.log('WARN', tag + ' 第 ' + (attempt + 1) + '/' + (retries + 1) + ' 次无应答');
        }
      }
      /* 退避后再试：给设备留出恢复窗口（Flash 擦写/端点忙等瞬时状态） */
      if (attempt < retries) { await sleep(20 + 40 * attempt); }
    }
    throw lastErr;
  };

  /* ---------------- 业务命令 ---------------- */

  /** → { mode, fwRev, vid, pid }；mode：0=App / 1=IAP */
  HidOta.prototype.getInfo = async function (timeoutMs, retries) {
    var r = await this.request(SUB.GET_INFO, null, timeoutMs || 1200, (retries === undefined) ? 1 : retries);
    var d = r.data;
    return {
      mode: d.length > 0 ? d[0] : MODE_APP,
      fwRev: d.length >= 3 ? (d[1] | (d[2] << 8)) : 0,
      vid: d.length >= 5 ? (d[3] | (d[4] << 8)) : 0,
      pid: d.length >= 7 ? (d[5] | (d[6] << 8)) : 0
    };
  };

  /** 通知 App 释放残留按键（方案A：不复位、不应答；FW_* 收包仍在 App 内完成） */
  HidOta.prototype.enterOta = async function () {
    try {
      await this._send(SUB.ENTER_OTA, null);
    } catch (e) {
      /* 设备可能在报告落地瞬间即复位，导致传输报错——视为已触发 */
      this.log('WARN', 'ENTER_OTA 传输报错（设备可能已复位）：' + e.message);
    }
  };

  HidOta.prototype.fwBegin = async function (totalLen, crc) {
    var a = new Uint8Array(6);
    a[0] = totalLen & 0xFF; a[1] = (totalLen >>> 8) & 0xFF;
    a[2] = (totalLen >>> 16) & 0xFF; a[3] = (totalLen >>> 24) & 0xFF;
    a[4] = crc & 0xFF; a[5] = (crc >> 8) & 0xFF;
    var r = await this.request(SUB.FW_BEGIN, a, 1500, 1);
    return r.data.length ? r.data[0] : 0;
  };

  HidOta.prototype.fwData = async function (offset, data) {
    var a = new Uint8Array(4 + data.length);
    a[0] = offset & 0xFF; a[1] = (offset >>> 8) & 0xFF;
    a[2] = (offset >>> 16) & 0xFF; a[3] = (offset >>> 24) & 0xFF;
    a.set(data, 4);
    /* 数据帧重试 4 次（含首试共 5 次）：HID 写偶发失败是长传输最常见的中断点，
     * 固件 bridge_ota_data 对重复帧幂等，重发安全，可显著提升整轮成功率。 */
    await this.request(SUB.FW_DATA, a, 1500, 4);
  };

  HidOta.prototype.fwEnd = async function () {
    await this.request(SUB.FW_END, null, 3000, 0);
  };

  HidOta.prototype.fwAbort = async function () {
    try { await this.request(SUB.FW_ABORT, null, 1000, 0); } catch (e) { /* ignore */ }
  };

  /**
   * 静默等待「期望模式的设备」出现并打开（不弹选择框）。
   * 依赖首次 requestDevice 授予的权限：getDevices() 直接拿回设备句柄。
   * 逐个试开后用 GET_INFO.mode 校验，不匹配则关闭并继续轮询。
   */
  HidOta.prototype.waitForMode = async function (expectedMode, timeoutMs) {
    var deadline = Date.now() + (timeoutMs || 20000);
    var announced = false;
    var tries = 0;
    var hot = [];   /* navigator.hid 'connect' 事件捕获的新句柄（复位后由浏览器推送） */
    var onConnect = function (e) { if (e && e.device && hot.indexOf(e.device) < 0) { hot.push(e.device); } };
    try { navigator.hid.addEventListener('connect', onConnect); } catch (e) { /* ignore */ }

    /* 关键：先把上一阶段的句柄**真正关闭**（_detach 只摘监听、不释放连接）。
     * 否则复位/重枚举后 getDevices() 仍返回旧对象且 opened===true，下面的
     * 「复用已打开句柄」分支会一直用它发报文——永远收不到新模式的应答，
     * 表现为「等待设备进入 X 模式超时」，须人工再点一次「选择设备」才恢复。 */
    try { await this.close(); } catch (e) { /* ignore */ }

    try {
      while (Date.now() < deadline) {
        tries++;
        var list = [];
        try { list = await navigator.hid.getDevices(); } catch (e) { list = []; }
        for (var k = 0; k < hot.length; k++) {
          if (list.indexOf(hot[k]) < 0) { list.push(hot[k]); }
        }
        var cand = [];
        for (var i = 0; i < list.length; i++) {
          if (this._isOtaIface(list[i])) { cand.push(list[i]); }
        }
        if (!announced || (tries % 5) === 0) {
          this.log('INFO', '等待设备进入 ' + modeName(expectedMode) + ' 模式…（可见 ' + list.length +
            '，其中 OTA 接口 ' + cand.length + '，第 ' + tries + ' 次）');
          announced = true;
        }
        for (var j = 0; j < cand.length; j++) {
          var dev = cand[j];
          /* 强制 close+open 重新取连接，绝不信任 opened 标志（见上文注释）。 */
          try { if (dev.opened) { await dev.close(); } } catch (e) { /* ignore */ }
          try { await dev.open(); } catch (e) { continue; }
          this._attach(dev);
          var info = null;
          /* 设备刚复位/重枚举后首包 IN 极易丢失；重试 2 次（不 close 重开，只重发）。 */
          try { info = await this.getInfo(1000, 2); } catch (e) { info = null; }
          if (info && info.mode === expectedMode) {
            this._attach(dev);   /* getInfo 里未改动 attach，保险起见幂等重挂 */
            return { device: dev, info: info };
          }
          this._detach();
          try { await dev.close(); } catch (e) { /* ignore */ }
        }
        await sleep(200);
      }
      throw new OtaTimeout('等待设备进入 ' + modeName(expectedMode) + ' 模式超时');
    } finally {
      try { navigator.hid.removeEventListener('connect', onConnect); } catch (e) { /* ignore */ }
    }
  };

  /** 准备镜像（补齐到 CHUNK(24B) 整数倍 + 整包 CRC）
   *  必须按 CHUNK 而非 4B 补齐：HID OUT 报告固定 32B，设备无法从报告长度判断末帧
   *  的真实数据量；若末帧短于 24B，剩余字节会被 USB 层零填充，设备将把零填充当作
   *  有效数据写入并计入 CRC → FW_END 校验失败。补齐到 24B 可保证每帧恒为 24B 有效。 */
  HidOta.prepare = function (raw) {
    if (raw.length > MAX_IMAGE) {
      throw new OtaError('镜像 ' + raw.length + 'B 超过上限 216KB');
    }
    var pad = (CHUNK - (raw.length % CHUNK)) % CHUNK;
    var total = raw.length + pad;
    var data = new Uint8Array(total);
    data.set(raw, 0);
    if (pad) { data.fill(0xFF, raw.length); }
    return { raw: raw, data: data, rawLen: raw.length, total: total, crc: global.OtaCrc.crc16(data) };
  };

  /** 整包升级（App 直接收包，无需先切 IAP）：FW_BEGIN → FW_DATA 循环 → FW_END（SPEC §5.6） */
  HidOta.prototype.flash = async function (raw, opts) {
    opts = opts || {};
    var prepared = HidOta.prepare(raw);
    this.abortRequested = false;

    this.log('INFO', '镜像 原始 ' + prepared.rawLen + 'B → 补齐 ' + prepared.total +
      'B，CRC 0x' + prepared.crc.toString(16).toUpperCase().padStart(4, '0'));

    var chunkMax = await this.fwBegin(prepared.total, prepared.crc);
    if (!chunkMax) { throw new OtaError('FW_BEGIN 被拒绝（chunk_max=0），IAP 当前不允许升级'); }
    var n = Math.min(chunkMax, CHUNK);
    n -= n % 4;
    if (n <= 0) { n = 4; }
    this.log('OK', 'FW_BEGIN 通过，chunk_max=' + chunkMax + '，分片 ' + n + 'B/帧，共 ' +
      Math.ceil(prepared.total / n) + ' 帧');

    var data = prepared.data;
    for (var off = 0; off < prepared.total; off += n) {
      if (this.abortRequested) {
        await this.fwAbort();
        throw new OtaError('用户已中止升级');
      }
      await this.fwData(off, data.subarray(off, Math.min(off + n, prepared.total)));
      this.onProgress(Math.min(off + n, prepared.total), prepared.total);
    }

    this.log('INFO', '数据发送完毕，发 FW_END 触发校验与搬运');
    try {
      await this.fwEnd();
      this.log('OK', 'FW_END 已应答，设备将搬运并复位');
    } catch (e) {
      if (e instanceof OtaTimeout || (e instanceof OtaError && e.message.indexOf('未连接') >= 0)) {
        this.log('WARN', 'FW_END 无应答 —— 设备已复位搬运（视为成功）');
      } else {
        throw e;
      }
    }
    return prepared;
  };

  HidOta.prototype.abort = function () { this.abortRequested = true; };

  function modeName(mode) { return mode === MODE_IAP ? 'IAP/OTA' : 'App'; }
  function hex(u8) {
    if (!u8 || !u8.length) { return ''; }
    var s = [];
    for (var i = 0; i < u8.length; i++) { s.push(u8[i].toString(16).toUpperCase().padStart(2, '0')); }
    return s.join(' ');
  }

  var api = {
    HidOta: HidOta, SUB: SUB, USAGE_PAGE: USAGE_PAGE, REPORT_LEN: REPORT_LEN,
    CHUNK: CHUNK, MAX_IMAGE: MAX_IMAGE, MODE_APP: MODE_APP, MODE_IAP: MODE_IAP, modeName: modeName
  };
  global.OtaHid = api;
  if (typeof module === 'object' && module.exports) { module.exports = api; }
})(typeof window !== 'undefined' ? window : globalThis);
