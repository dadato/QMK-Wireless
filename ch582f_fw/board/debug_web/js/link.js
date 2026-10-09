/* 链路层 —— WebSerial 收发 / 出帧间隔 / ACK 匹配 / 心跳 / 事件自动应答
 *
 * 行为对齐 tools/sk32sim/link.py（SK32 角色）：
 *   - 出帧间隔 >= max(5ms, 本帧线上时间 + 0.5ms)（8N1 → 10 bit/byte）
 *   - 每 1000ms 发 CMD_PING；收到桥的 PING（flags=0）回 EVT_PONG（flags=ACK, seq 取原帧）
 *   - 控制命令按 (cmd, seq) 匹配 ACK/ERR 应答；默认超时 600ms、重试 1 次
 *   - 事件帧（0x80..0x87）带 ACK_REQ 时自动回 ACK（cmd/seq 取原帧）
 *   - 统计：TX/RX/CRC 错/超时/ERR/事件··/PING··/DUP
 */
(function (global) {
  'use strict';

  var F = global.DbgFrame || require('./frame.js');

  var BAUD = 921600;             /* 默认波特率（SPEC §4.1：模组默认 921600 8N1；可由 opts.baud 覆盖） */
  var MIN_GAP_MS = 5;           /* SPEC §4.5 */
  var HEALTH_WINDOW_MS = 3500;  /* SPEC §5.2：最近收到桥 PING 的窗口 */

  function nowMsAbs() {
    return (global.performance && global.performance.now)
      ? global.performance.now() : Date.now();
  }
  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  /** Promise 超时包装：超时或失败返回 false（不抛），完成返回 true */
  function withTimeout(p, ms) {
    return new Promise(function (resolve) {
      var done = false;
      var t = setTimeout(function () { if (!done) { done = true; resolve(false); } }, ms);
      p.then(function () { if (!done) { done = true; clearTimeout(t); resolve(true); } },
        function () { if (!done) { done = true; clearTimeout(t); resolve(false); } });
    });
  }

  function Stats() {
    this.txFrames = 0;
    this.rxFrames = 0;
    this.crcErrors = 0;
    this.pingRx = 0;
    this.pongRx = 0;
    this.events = 0;
    this.timeouts = 0;
    this.errorsRx = 0;
  }

  function Link(opts) {
    opts = opts || {};
    var self = this;
    this.log = opts.log || function () {};
    this.onFrame = opts.onFrame || function () {};
    this.onCrcError = opts.onCrcError || function () {};
    this.onEvent = opts.onEvent || function () {};
    this.onDisconnect = opts.onDisconnect || function () {};

    this.baud = opts.baud || BAUD;
    this.gapMs = (opts.gapMs === undefined) ? MIN_GAP_MS : opts.gapMs;
    this.heartbeatMs = (opts.heartbeatMs === undefined) ? 1000 : opts.heartbeatMs;
    this.autoAck = (opts.autoAck === undefined) ? true : opts.autoAck;

    this.stats = new Stats();
    this.port = null;
    this.reader = null;
    this.writer = null;
    this.parser = new F.Parser(function (raw) { self._onCrcError(raw); });
    this.pending = {};          /* "cmd:seq" → {resolve,reject,timer} */
    this.seq = 0;
    this.lastTx = 0;
    this.closed = true;
    this.closing = false;
    this.lastPingRxMs = null;
    this.lastPongMs = null;
    this._hbTimer = null;
    this.localHbEnabled = false;  /* 本机心跳（Web 侧 1s PING）开关：单向机制下默认关闭 */
    this._eventQ = [];
    this._waiters = [];
    this._rxTask = null;
    this.lastTxRecord = null;   /* 最近一帧 TX 记录（供命令台回显原始帧） */
    this._t0 = nowMsAbs();
  }

  Link.isSupported = function () {
    return typeof navigator !== 'undefined' && 'serial' in navigator;
  };

  /* ---------------- 时间 / 序号 ---------------- */
  Link.prototype.nowMs = function () { return nowMsAbs() - this._t0; };

  Link.prototype.nextSeq = function () {
    this.seq = (this.seq + 1) & 0xFF;
    return this.seq;
  };

  Link.prototype.resetClock = function () { this._t0 = nowMsAbs(); };

  /* ---------------- 连接生命周期 ---------------- */
  /** @param {SerialPort} [port] 不传则弹选择框 */
  Link.prototype.connect = async function (port) {
    if (!Link.isSupported()) { throw new F.BridgeError('当前浏览器不支持 WebSerial'); }
    var p = port || (await navigator.serial.requestPort());
    await p.open({ baudRate: this.baud, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none' });
    this.port = p;
    this.closed = false;
    this.closing = false;
    this.resetClock();
    this.writer = p.writable.getWriter();
    this.reader = p.readable.getReader();
    this._rxTask = this._rxLoop();
    if (this.localHbEnabled) { this._startHeartbeat(); }
    this.log('OK', '已打开串口 @ ' + this.baud + ' 8N1（心跳 ' + this.heartbeatMs + 'ms）');
    return p;
  };

  Link.prototype._startHeartbeat = function () {
    var self = this;
    if (this._hbTimer) { return; }
    this._hbTimer = setInterval(function () {
      if (self.closed || !self.localHbEnabled) { return; }
      try { self.ping(); } catch (e) { /* 写失败由 rx/teardown 处理 */ }
    }, this.heartbeatMs);
  };

  /** 启停本机心跳（Web 侧周期 PING）。仅影响本端发心跳，不改桥侧 SET_HEARTBEAT 状态。
   *  @returns {boolean} 当前是否启用 */
  Link.prototype.setLocalHeartbeat = function (enabled) {
    this.localHbEnabled = (enabled !== false);
    if (!this.localHbEnabled) {
      if (this._hbTimer) { clearInterval(this._hbTimer); this._hbTimer = null; }
      this.log('WARN', '本机心跳已停发（不再周期发送 PING）');
    } else {
      if (!this.closed) { this._startHeartbeat(); }
      this.log('OK', '本机心跳已恢复（' + this.heartbeatMs + 'ms）');
    }
    return this.localHbEnabled;
  };

  Link.prototype.close = async function () {
    if (this.closing) { return; }
    this.closing = true;
    if (this._hbTimer) { clearInterval(this._hbTimer); this._hbTimer = null; }
    this.closed = true;
    this._rejectAll(new F.BridgeError('串口已关闭'));
    try { if (this.reader) { await this.reader.cancel(); } } catch (e) { /* ignore */ }
    try { if (this.reader) { this.reader.releaseLock(); } } catch (e) { /* ignore */ }
    try { if (this.writer) { this.writer.releaseLock(); } } catch (e) { /* ignore */ }
    this.reader = null;
    this.writer = null;
    try { if (this.port) { await this.port.close(); } } catch (e) { /* ignore */ }
    this.port = null;
  };

  Link.prototype.isConnected = function () { return !this.closed && !!this.port; };

  /* ---------------- 发送 ---------------- */
  Link.prototype._writeFrame = async function (cmd, seq, flags, payload, timeoutMs) {
    if (this.closed || !this.writer) { throw new F.BridgeError('串口已关闭'); }
    var p = payload || new Uint8Array(0);
    var raw = F.encode(cmd, seq, flags, p);
    /* D14：出帧间隔 >= max(5ms, 线上时间 + 0.5ms) */
    var wireMs = raw.length * 10 / this.baud * 1000;
    var need = Math.max(this.gapMs, wireMs + 0.5);
    var wait = need - (nowMsAbs() - this.lastTx);
    if (wait > 0) { await sleep(wait); }
    var wp = this.writer.write(raw);
    this.lastTx = nowMsAbs();
    this.stats.txFrames++;
    this._emit('TX', new F.Frame(cmd, seq, flags, p), raw, 'TX');
    /* 无 timeoutMs：照常等待写完（绝大多数帧）。
     * 有 timeoutMs：超时未完成返回 false —— 典型是发 ENTER_BOOT 后模组立即软复位，
     * WCH-Link 虚串口的写完成通知丢失，Chrome 的 write() promise 永不 settle。
     * 此时字节其实已送达（模组确实复位了），调用方据此停止连发（见 requestBurst）。 */
    if (!timeoutMs) { await wp; return true; }
    return withTimeout(wp, timeoutMs);
  };

  /** 原始组帧（供原始帧编辑器 / 危险操作；seq 由调用方指定） */
  Link.prototype.sendRaw = function (cmd, seq, flags, payload) {
    return this._writeFrame(cmd, seq === undefined || seq === null ? this.nextSeq() : seq,
      flags || 0, payload || new Uint8Array(0));
  };

  /** 快速路径（无 ACK）：自动递增 seq、flags=0 */
  Link.prototype.sendFast = function (cmd, payload) {
    return this._writeFrame(cmd, this.nextSeq(), 0, payload || new Uint8Array(0));
  };

  /** 心跳 PING（不等待 PONG；PONG 计数在 RX 处理） */
  Link.prototype.ping = function () {
    return this._writeFrame(F.CMD.PING, this.nextSeq(), 0, new Uint8Array(0));
  };

  /** 控制命令：发送并等待 (cmd,seq) 匹配的 ACK/ERR 帧 */
  Link.prototype.request = async function (cmd, payload, timeoutMs, retries) {
    timeoutMs = timeoutMs || 600;
    retries = (retries === undefined) ? 1 : retries;
    var seq = this.nextSeq();
    var key = cmd + ':' + seq;
    var lastErr = null;
    for (var attempt = 0; attempt <= retries; attempt++) {
      var p = this._waitReply(key, timeoutMs);
      try {
        await this._writeFrame(cmd, seq, 0, payload || new Uint8Array(0));
      } catch (e) {
        this._dropPending(key);
        throw e;
      }
      try {
        var fr = await p;
        if (fr.isErr()) {
          throw new F.BridgeError('固件返回 ERR：' + F.errCodeName(fr.errCode()) +
            '（' + fr.name() + '）', fr.errCode());
        }
        return fr;
      } catch (e) {
        lastErr = e;
        if (e instanceof F.BridgeError) { throw e; }
        this.log('WARN', F.cmdName(cmd) + ' seq=' + seq + ' 第 ' + (attempt + 1) + ' 次无应答');
      }
    }
    throw lastErr || new F.BridgeTimeout('请求失败');
  };

  /** 原始帧发送并等待匹配 (cmd,seq) 应答（供原始帧编辑器，seq 由调用方指定） */
  Link.prototype.requestRaw = async function (cmd, seq, flags, payload, timeoutMs, retries) {
    timeoutMs = timeoutMs || 600;
    retries = (retries === undefined) ? 1 : retries;
    var key = cmd + ':' + seq;
    var lastErr = null;
    for (var attempt = 0; attempt <= retries; attempt++) {
      var p = this._waitReply(key, timeoutMs);
      try {
        await this._writeFrame(cmd, seq, flags || 0, payload || new Uint8Array(0));
      } catch (e) {
        this._dropPending(key);
        throw e;
      }
      try {
        var fr = await p;
        if (fr.isErr()) {
          throw new F.BridgeError('固件返回 ERR：' + F.errCodeName(fr.errCode()) +
            '（' + fr.name() + '）', fr.errCode());
        }
        return fr;
      } catch (e) {
        lastErr = e;
        if (e instanceof F.BridgeError) { throw e; }
      }
    }
    throw lastErr || new F.BridgeTimeout('请求失败');
  };

  /** 密集连发并等待应答（穿窗）：同一 (cmd,seq) 在 totalMs 内反复重发，
   * 直到收到 ACK/ERR 或超时。用于模组处于浅睡导致单发命中率极低的场景
   * （镜像 ota_web/js/serial_ota.js 的 requestBurst；SPEC §10.2）。
   * 每帧带写超时：模组复位后写完成通知丢失（返回 false）即提前停止连发。
   * @param {number} totalMs 连发总时长（默认 3000ms）
   * @param {number} gapMs   连发间隔（默认 1ms）
   */
  Link.prototype.requestBurst = async function (cmd, payload, totalMs, gapMs) {
    totalMs = totalMs || 3000;
    gapMs = (gapMs === undefined) ? 1 : gapMs;
    var seq = this.nextSeq();
    var key = cmd + ':' + seq;
    var p = this._waitReply(key, totalMs);
    p.catch(function () { /* 防未捕获拒绝：下方 await p 已处理 */ });
    var deadline = nowMsAbs() + totalMs;
    while (nowMsAbs() < deadline && this.pending[key]) {
      var ok;
      try {
        ok = await this._writeFrame(cmd, seq, 0, payload || new Uint8Array(0), 800);
      } catch (e) {
        this._dropPending(key);
        throw e;
      }
      if (!ok) {
        /* 写完成通知丢失 = 模组已复位（如 ENTER_BOOT 收到后即复位）。停止连发，
         * 但仍等队列里可能已到达的 ACK（在其等待窗口内）。 */
        break;
      }
      if (gapMs > 0) { await sleep(gapMs); }
    }
    var fr = await p;   /* 超时 → BridgeTimeout 上抛 */
    if (fr.isErr()) {
      throw new F.BridgeError('固件返回 ERR：' + F.errCodeName(fr.errCode()) +
        '（' + fr.name() + '）', fr.errCode());
    }
    return fr;
  };

  Link.prototype._waitReply = function (key, timeoutMs) {
    var self = this;
    return new Promise(function (resolve, reject) {
      var timer = setTimeout(function () {
        delete self.pending[key];
        self.stats.timeouts++;
        reject(new F.BridgeTimeout('等待应答超时（' + timeoutMs + 'ms）'));
      }, timeoutMs);
      self.pending[key] = { resolve: resolve, reject: reject, timer: timer };
    });
  };

  Link.prototype._dropPending = function (key) {
    var w = this.pending[key];
    if (w) { clearTimeout(w.timer); delete this.pending[key]; }
  };

  /* ---------------- 接收 ---------------- */
  Link.prototype._rxLoop = async function () {
    while (!this.closed) {
      var res;
      try {
        res = await this.reader.read();
      } catch (e) {
        if (!this.closed) {
          this.log('ERR', '串口读取中断：' + (e && e.message));
          this._teardown();
        }
        return;
      }
      if (res.done) { break; }
      if (!res.value || !res.value.length) { continue; }
      var frames = this.parser.feed(res.value);
      for (var i = 0; i < frames.length; i++) { this._handle(frames[i]); }
    }
    if (!this.closed) { this._teardown(); }
  };

  Link.prototype._onCrcError = function (raw) {
    this.stats.crcErrors++;
    this.log('WARN', 'CRC 错误：' + F.bytesToHex(raw));
    try { this.onCrcError(raw); } catch (e) { /* ignore */ }
  };

  Link.prototype._emit = function (dir, fr, raw, kind) {
    var rec = {
      dir: dir, t: this.nowMs(), frame: fr, raw: raw,
      kind: kind || fr.kind(),
      summary: global.DbgDecode ? global.DbgDecode.summarize(fr) : ''
    };
    if (dir === 'TX') { this.lastTxRecord = rec; }
    try { this.onFrame(rec); } catch (e) { /* ignore */ }
    return rec;
  };

  Link.prototype._handle = function (fr) {
    this.stats.rxFrames++;

    /* 桥的心跳 PING（flags=0）→ 回 PONG（镜像 SK32 行为） */
    if (fr.cmd === F.CMD.PING && fr.flags === 0) {
      this.stats.pingRx++;
      this.lastPingRxMs = this.nowMs();
      this._emit('RX', fr, null, 'RX');
      this._writeFrame(F.EVT.PONG, fr.seq, F.FLAG_ACK, new Uint8Array(0)).catch(function () {});
      return;
    }

    /* ACK / ERR 应答帧 */
    if (fr.isAck() || fr.isErr()) {
      this._emit('RX', fr, null, fr.kind());
      if (fr.cmd === F.EVT.PONG) { this.stats.pongRx++; this.lastPongMs = this.nowMs(); }
      if (fr.isErr()) {
        this.stats.errorsRx++;
        this.log('WARN', 'ERR ' + F.errCodeName(fr.errCode()) + ' ← ' + fr.name() + ' (seq=' + fr.seq + ')');
      }
      var w = this.pending[fr.cmd + ':' + fr.seq];
      if (w) { clearTimeout(w.timer); delete this.pending[fr.cmd + ':' + fr.seq]; w.resolve(fr); }
      return;
    }

    /* 事件帧 */
    if (fr.isEvent()) {
      this.stats.events++;
      this._emit('RX', fr, null, 'EVT');
      if ((fr.flags & F.FLAG_ACK_REQ) && this.autoAck) {
        this._writeFrame(fr.cmd, fr.seq, F.FLAG_ACK, new Uint8Array(0)).catch(function () {});
      }
      this._pushEvent(fr);
      try { this.onEvent(fr); } catch (e) { this.log('ERR', 'onEvent 回调异常：' + e.message); }
      return;
    }

    this._emit('RX', fr, null, 'RX');
    this.log('WARN', '未处理帧 cmd=' + fr.name() + ' seq=' + fr.seq +
      ' flags=0x' + fr.flags.toString(16) + ' payload=' + fr.hex());
  };

  /* ---------------- 事件等待 ---------------- */
  Link.prototype._pushEvent = function (fr) {
    this._eventQ.push(fr);
    if (this._eventQ.length > 512) { this._eventQ.splice(0, 256); }
    this._resolveWaiters();
  };

  Link.prototype._resolveWaiters = function () {
    for (var i = this._waiters.length - 1; i >= 0; i--) {
      var w = this._waiters[i];
      for (var j = 0; j < this._eventQ.length; j++) {
        var fr = this._eventQ[j];
        if ((w.cmd === null || fr.cmd === w.cmd) && (!w.pred || w.pred(fr))) {
          this._eventQ.splice(j, 1);
          this._waiters.splice(i, 1);
          clearTimeout(w.timer);
          w.resolve(fr);
          break;
        }
      }
    }
  };

  Link.prototype.waitEvent = function (timeoutMs, cmd, pred) {
    var self = this;
    cmd = (cmd === undefined) ? null : cmd;
    /* 先查已缓存事件 */
    for (var j = 0; j < this._eventQ.length; j++) {
      var fr = this._eventQ[j];
      if ((cmd === null || fr.cmd === cmd) && (!pred || pred(fr))) {
        this._eventQ.splice(j, 1);
        return Promise.resolve(fr);
      }
    }
    return new Promise(function (resolve, reject) {
      var w = { cmd: cmd, pred: pred || null, resolve: resolve, reject: reject, timer: null };
      w.timer = setTimeout(function () {
        var k = self._waiters.indexOf(w);
        if (k >= 0) { self._waiters.splice(k, 1); }
        reject(new F.BridgeTimeout('等待事件超时（cmd=' + (cmd === null ? 'any' : F.eventName(cmd)) + '）'));
      }, timeoutMs);
      self._waiters.push(w);
    });
  };

  /* ---------------- 健康度 ---------------- */
  Link.prototype.healthOk = function (windowMs) {
    var win = (windowMs === undefined) ? HEALTH_WINDOW_MS : windowMs;
    if (this.lastPingRxMs === null) { return false; }
    return (this.nowMs() - this.lastPingRxMs) <= win;
  };

  Link.prototype.statsSnapshot = function () {
    var s = this.stats;
    return {
      txFrames: s.txFrames, rxFrames: s.rxFrames, crcErrors: s.crcErrors,
      pingRx: s.pingRx, pongRx: s.pongRx, events: s.events,
      timeouts: s.timeouts, errorsRx: s.errorsRx
    };
  };

  /* ---------------- 断线处理 ---------------- */
  Link.prototype._rejectAll = function (err) {
    for (var k in this.pending) {
      if (Object.prototype.hasOwnProperty.call(this.pending, k)) {
        clearTimeout(this.pending[k].timer);
        this.pending[k].reject(err);
        delete this.pending[k];
      }
    }
    for (var i = 0; i < this._waiters.length; i++) {
      clearTimeout(this._waiters[i].timer);
      this._waiters[i].reject(err);
    }
    this._waiters = [];
  };

  Link.prototype._teardown = function () {
    if (this.closed) { return; }
    this.closed = true;
    if (this._hbTimer) { clearInterval(this._hbTimer); this._hbTimer = null; }
    this._rejectAll(new F.BridgeError('设备已断开'));
    try { this.onDisconnect(); } catch (e) { /* ignore */ }
  };

  var api = { Link: Link, Stats: Stats, BAUD: BAUD, MIN_GAP_MS: MIN_GAP_MS, HEALTH_WINDOW_MS: HEALTH_WINDOW_MS };
  global.DbgLink = api;
  if (typeof module === 'object' && module.exports) { module.exports = api; }
})(typeof window !== 'undefined' ? window : globalThis);
