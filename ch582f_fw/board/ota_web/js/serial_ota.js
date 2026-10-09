/* 通路 B：键盘侧 CH582 模组 —— WebSerial（USB-TTL 直连模组 UART1，默认 921600 8N1，波特率可配）
 *
 * 复用固件已实现的 UART OTA 协议（SPEC §4）：
 *   FW_BEGIN(total,crc) → 回 chunk_max；FW_DATA(offset,data) 逐片；FW_END → 设备复位搬运。
 * 行为对齐 tools/sk32sim/link.py：出帧间隔 >= max(5ms, 线上时间+0.5ms)；
 *   (cmd,seq) 匹配 ACK/ERR；事件帧带 ACK_REQ 自动回 ACK；对端 PING 回 PONG。
 */
(function (global) {
  'use strict';

  var F = global.OtaFrame;
  var CMD = F.CMD;
  var EVT = F.EVT;
  var FLAG = { ACK_REQ: F.FLAG_ACK_REQ, ACK: F.FLAG_ACK, ERR: F.FLAG_ERR };
  var OtaError = F.OtaError;
  var OtaTimeout = F.OtaTimeout;

  var BAUD = 921600;           /* 默认波特率；实际使用实例级 this.baud（构造时 opts.baud 覆盖） */
  var MIN_GAP_MS = 5;
  var FW_GAP_MS = 20;   /* FW_DATA 有效出帧间隔下限（实测阈值，见 fwData 注释） */
  var CHUNK_MAX = 60;          /* BRIDGE_FW_CHUNK_MAX */
  var MAX_IMAGE = 216 * 1024;  /* BRIDGE_FW_MAX_LEN */

  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  /** 给 promise 套一个超时：按时完成返回 true，超时/失败返回 false（不抛出） */
  function withTimeout(p, ms) {
    return new Promise(function (resolve) {
      var done = false;
      var t = setTimeout(function () { if (!done) { done = true; resolve(false); } }, ms);
      p.then(function () { if (!done) { done = true; clearTimeout(t); resolve(true); } },
        function () { if (!done) { done = true; clearTimeout(t); resolve(false); } });
    });
  }

  function SerialOta(opts) {
    opts = opts || {};
    this.log = opts.log || function () {};
    this.onProgress = opts.onProgress || function () {};
    this.onDisconnect = opts.onDisconnect || function () {};
    this.baud = (opts.baud > 0) ? (opts.baud | 0) : BAUD;  /* 本次连接的波特率 */
    this.port = null;
    this.reader = null;
    this.writer = null;
    this.parser = new F.Parser();
    this.pending = {};
    this.seq = 0;
    this.lastTx = 0;
    this.closed = true;
    this.abortRequested = false;
    this._rebuilding = false;   /* 重建管道期间抑制 _rxLoop 触发的断开回调 */
    this._rxTask = null;
  }

  SerialOta.isSupported = function () {
    return typeof navigator !== 'undefined' && 'serial' in navigator;
  };

  /* ---------------- 连接生命周期 ---------------- */

  /** @param {SerialPort} [port] 不传则弹选择框 */
  SerialOta.prototype.connect = async function (port) {
    if (!SerialOta.isSupported()) { throw new OtaError('当前浏览器不支持 WebSerial'); }
    var p = port || (await navigator.serial.requestPort());
    await p.open({ baudRate: this.baud, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none' });
    this.port = p;
    this.closed = false;
    this.abortRequested = false;
    this.writer = p.writable.getWriter();
    this.reader = p.readable.getReader();
    this._rxTask = this._rxLoop();
    return p;
  };

  SerialOta.prototype.close = async function () {
    if (this._closing) { return; }
    this._closing = true;
    this.closed = true;
    try { if (this.reader) { await this.reader.cancel(); } } catch (e) { /* ignore */ }
    try { if (this.reader) { this.reader.releaseLock(); } } catch (e) { /* ignore */ }
    try { if (this.writer) { this.writer.releaseLock(); } } catch (e) { /* ignore */ }
    this.reader = null;
    this.writer = null;
    try { if (this.port) { await this.port.close(); } } catch (e) { /* ignore */ }
  };

  SerialOta.prototype.isConnected = function () { return !this.closed && !!this.port; };

  /* ---------------- 收发 ---------------- */

  SerialOta.prototype._nextSeq = function () {
    this.seq = (this.seq + 1) & 0xFF;
    return this.seq;
  };

  SerialOta.prototype._writeFrame = async function (cmd, seq, flags, payload, timeoutMs) {
    if (this.closed || !this.writer) { throw new OtaError('串口已关闭'); }
    var raw = F.encode(cmd, seq, flags, payload);
    /* D14：出帧间隔 >= max(5ms, 线上时间 + 0.5ms) */
    var wireMs = raw.length * 10 / this.baud * 1000;
    var need = Math.max(MIN_GAP_MS, wireMs + 0.5);
    var wait = need - (performance.now() - this.lastTx);
    if (wait > 0) { await sleep(wait); }
    var p = this.writer.write(raw);
    this.lastTx = performance.now();
    /* 无 timeoutMs：照常等待写完（绝大多数帧）。
     * 有 timeoutMs：超时未完成返回 false —— 典型是发 ENTER_BOOT 后模组立即软复位，
     * WCH-Link 虚串口的写完成通知丢失，Chrome 的 write() promise 永不 settle
     * （host 侧 pyserial 有 write_timeout=2s 故能脱身）。此时字节其实已送达
     * （模组确实复位了），调用方据此重建串口即可（见 enterBoot）。 */
    if (!timeoutMs) { await p; return true; }
    return withTimeout(p, timeoutMs);
  };

  SerialOta.prototype._waitReply = function (key, timeoutMs) {
    var self = this;
    return new Promise(function (resolve, reject) {
      self.pending[key] = {
        resolve: resolve,
        reject: reject,
        timer: setTimeout(function () {
          delete self.pending[key];
          reject(new OtaTimeout('等待应答超时 (' + timeoutMs + 'ms)'));
        }, timeoutMs)
      };
    });
  };

  /** 发送并等待 (cmd,seq) 匹配的 ACK/ERR 帧 */
  SerialOta.prototype.request = async function (cmd, payload, timeoutMs, retries) {
    timeoutMs = timeoutMs || 600;
    retries = (retries === undefined) ? 1 : retries;
    var seq = this._nextSeq();
    var key = cmd + ':' + seq;
    var lastErr = null;
    for (var attempt = 0; attempt <= retries; attempt++) {
      var p = this._waitReply(key, timeoutMs);
      try {
        await this._writeFrame(cmd, seq, 0, payload || new Uint8Array(0));
      } catch (e) {
        delete this.pending[key];
        throw e;
      }
      try {
        var fr = await p;
        if (fr.isErr()) {
          throw new OtaError('固件返回 ERR：' + F.errCodeName(fr.errCode()) + ' (' + fr.name() + ')',
            fr.errCode());
        }
        return fr;
      } catch (e) {
        lastErr = e;
        if (e instanceof OtaError) { throw e; }
        this.log('WARN', cmd + ' seq=' + seq + ' 第 ' + (attempt + 1) + ' 次无应答');
      }
    }
    throw lastErr;
  };

  /** 密集连发并等待 (cmd,seq) 匹配的 ACK/ERR（App 态浅睡专用）。
   * 根因：模组 App 态每 20ms 进浅睡，PC→模组的帧首字节会被唤醒过程吞掉，
   * 且清醒窗口极短，单发/少量重试命中率仅约 0.4%~1%/帧（自建探针实测）。
   * 解法：同一 seq 密集连发，以统计方式穿过浅睡窗口；UART 收侧无 seq 去重
   * （见 core/bridge_uart.c），重复帧安全。收到任一干净 ACK 立即返回。 */
  SerialOta.prototype.requestBurst = async function (cmd, payload, totalMs, gapMs) {
    totalMs = totalMs || 3000;
    gapMs = (gapMs === undefined) ? 1 : gapMs;
    var seq = this._nextSeq();
    var key = cmd + ':' + seq;
    var p = this._waitReply(key, totalMs);
    p.catch(function () { /* 防未捕获拒绝：下方 await p 已处理 */ });
    var deadline = performance.now() + totalMs;
    while (performance.now() < deadline && this.pending[key]) {
      var ok;
      try {
        ok = await this._writeFrame(cmd, seq, 0, payload || new Uint8Array(0), 800);
      } catch (e) {
        delete this.pending[key];
        throw e;
      }
      if (!ok) {
        /* 写完成通知丢失 = 模组已复位（如 ENTER_BOOT 收到后即复位）。停止连发，
         * 但仍等队列里可能已到达的 ACK（在其等待窗口内）。 */
        break;
      }
      if (gapMs > 0) { await sleep(gapMs); }
    }
    var fr = await p;   /* 超时 → OtaTimeout 上抛 */
    if (fr.isErr()) {
      throw new OtaError('固件返回 ERR：' + F.errCodeName(fr.errCode()) + ' (' + fr.name() + ')',
        fr.errCode());
    }
    return fr;
  };

  /** 仅密集连发、不等待应答（用于 ENTER_BOOT 这类无应答命令）。
   * 每帧带写超时保护：模组一旦复位，WCH-Link 虚串口的写完成通知丢失
   * （_writeFrame 返回 false）即判为「帧已送达」提前结束；写抛异常亦结束。
   * @returns {number} 实际发送帧数 */
  SerialOta.prototype.sendBurst = async function (cmd, payload, totalMs, gapMs) {
    totalMs = totalMs || 2500;
    gapMs = (gapMs === undefined) ? 1 : gapMs;
    var seq = this._nextSeq();
    var deadline = performance.now() + totalMs;
    var sent = 0;
    while (performance.now() < deadline) {
      var ok;
      try {
        ok = await this._writeFrame(cmd, seq, 0, payload || new Uint8Array(0), 800);
      } catch (e) {
        break;
      }
      sent++;
      if (!ok) { break; }   /* 复位导致写完成通知丢失 → 帧已送达 */
      if (gapMs > 0) { await sleep(gapMs); }
    }
    return sent;
  };

  SerialOta.prototype._rxLoop = async function () {
    while (!this.closed) {
      var res;
      try {
        res = await this.reader.read();
      } catch (e) {
        if (!this.closed && !this._rebuilding) {
          this.log('ERR', '串口读取中断：' + e.message + '（设备可能已复位）');
          this._teardown();
        }
        return;
      }
      if (res.done) { break; }
      if (!res.value || !res.value.length) { continue; }
      var frames = this.parser.feed(res.value);
      for (var i = 0; i < frames.length; i++) { this._handle(frames[i]); }
    }
    if (!this.closed && !this._rebuilding) { this._teardown(); }
  };

  /** 重建 USB 管道：放弃当前 reader/writer，用同一 SerialPort 重新 open。
   * 用于跨「模组软复位」边界（见 enterBoot / _writeFrame）：复位会让 WCH-Link
   * 虚串口的写完成通知丢失，旧 writer 上的写 promise 可能永久 pending，
   * 必须整体换掉才能继续收发。 */
  SerialOta.prototype._rebuildTransport = async function () {
    var p = this.port;
    if (!p) { throw new OtaError('串口未连接'); }
    this._rebuilding = true;
    try {
      /* 读取侧：cancel 令挂起的 read() 以 {done:true} 结束 */
      try { if (this.reader) { await withTimeout(this.reader.cancel(), 800); } } catch (e) { /* ignore */ }
      try { if (this.reader) { this.reader.releaseLock(); } } catch (e) { /* ignore */ }
      this.reader = null;
      /* 写入侧：abort 丢弃可能永久 pending 的写 */
      try { if (this.writer) { await withTimeout(this.writer.abort('rebuild'), 800); } } catch (e) { /* ignore */ }
      try { if (this.writer) { this.writer.releaseLock(); } } catch (e) { /* ignore */ }
      this.writer = null;
      /* 关端口 → 重开 */
      try { await withTimeout(p.close(), 1500); } catch (e) { /* ignore */ }
      await p.open({ baudRate: this.baud, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none' });
      this.writer = p.writable.getWriter();
      this.reader = p.readable.getReader();
      this.parser = new F.Parser();
      this.lastTx = 0;
      this.closed = false;
      this._rxTask = this._rxLoop();
      return true;
    } finally {
      this._rebuilding = false;
    }
  };

  SerialOta.prototype._teardown = function () {
    if (this.closed || this._rebuilding) { return; }
    this.closed = true;
    for (var k in this.pending) {
      if (Object.prototype.hasOwnProperty.call(this.pending, k)) {
        clearTimeout(this.pending[k].timer);
        this.pending[k].reject(new OtaError('设备已断开'));
        delete this.pending[k];
      }
    }
    this.onDisconnect();
  };

  SerialOta.prototype._handle = function (fr) {
    /* 对端心跳 PING（flags=0）→ 回 PONG（镜像 link.py，固件不依赖） */
    if (fr.cmd === CMD.PING && fr.flags === 0) {
      this._writeFrame(EVT.PONG, fr.seq, FLAG.ACK, new Uint8Array(0)).catch(function () {});
      return;
    }
    /* ACK / ERR 应答帧：按 (cmd,seq) 派发 */
    if (fr.isAck() || fr.isErr()) {
      if (fr.isErr()) {
        this.log('WARN', '← ERR ' + F.errCodeName(fr.errCode()) + ' for ' + fr.name() + ' (seq=' + fr.seq + ')');
      }
      var key = fr.cmd + ':' + fr.seq;
      var waiter = this.pending[key];
      if (waiter) {
        clearTimeout(waiter.timer);
        delete this.pending[key];
        waiter.resolve(fr);
      }
      return;
    }
    /* 事件帧：带 ACK_REQ 则自动回 ACK，并记日志（不影响 OTA 主流程） */
    if (fr.isEvent()) {
      if (fr.flags & FLAG.ACK_REQ) {
        this._writeFrame(fr.cmd, fr.seq, FLAG.ACK, new Uint8Array(0)).catch(function () {});
      }
      this.log('INFO', '← ' + fr.name() + ' ' + fr.hex() + ' (已自动应答)');
      return;
    }
    this.log('WARN', '← 未处理帧 cmd=' + fr.name() + ' seq=' + fr.seq + ' flags=0x' +
      fr.flags.toString(16) + ' payload=' + fr.hex());
  };

  /* ---------------- 业务命令 ---------------- */

  SerialOta.prototype.getVersion = async function () {
    /* App 态浅睡，单发极难命中 → 密集连发（3s 内约数百帧）取任一干净 ACK */
    var fr = await this.requestBurst(CMD.GET_VERSION, null, 3000, 1);
    return [fr.payload[0] || 0, fr.payload[1] || 0, fr.payload[2] || 0];
  };

  /** 跳进常驻 BOOT 的 UART 收包模式（A 方案：先跳 BOOT 再升级）。
   * App 收到 CMD_ENTER_BOOT 后**先回 ACK、再写 UART_IAP 标志 + 软复位**，
   * 重启后键盘 BOOT（iap_main.c）见 flag==UART_IAP 驻留收包。
   * 关键 1：App 态浅睡使单发命中率仅约 0.4%~1%/帧 → 密集连发穿窗；
   * 关键 2：改用「等 ACK 即停发」（requestBurst）——收到应答立即停止连发，
   *   从根源消除复位后仍在途的 ENTER_BOOT 落到 BOOT（只认 GET_ 系列、FW_ 系列
   *   与 PING）而产生的 ERR PARAM 风暴；
   * 关键 3：模组软复位不改变 WCH-Link 虚串口的 USB 枚举，故默认沿用现有管道，
   *   用 GET_STATUS(mode==5/OTA) 确认已进 BOOT；确认不到才回退重建管道。 */
  SerialOta.prototype.enterBoot = async function () {
    try {
      await this.requestBurst(CMD.ENTER_BOOT, null, 2500, 1);
      this.log('OK', 'ENTER_BOOT 已应答，等待复位并进入 BOOT…');
    } catch (e) {
      this.log('WARN', 'ENTER_BOOT 未收到应答（' + e.message + '），仍尝试确认 BOOT…');
    }
    await sleep(400);            /* App 复位 + BOOT 初始化 + UART1 重新初始化 */

    if (await this.waitForBoot(2500)) { return; }

    this.log('WARN', '未收到 BOOT 应答，回退：重建串口管道后重试');
    try { await this._rebuildTransport(); } catch (e) { this.log('WARN', '重建串口失败：' + e.message); }
    await sleep(200);
    if (await this.waitForBoot(4000)) { return; }
    throw new OtaError('未能进入 BOOT：ENTER_BOOT 后 GET_STATUS 始终非 OTA');
  };

  /** 轮询确认已进入 BOOT：BOOT 恒回 GET_STATUS mode=5(OTA)，App 回真实 mode。
   * 每轮小 burst 连发（App 浅睡需穿窗；BOOT 就绪后即秒回）。返回 true 表示已进 BOOT。 */
  SerialOta.prototype.waitForBoot = async function (totalMs) {
    var deadline = performance.now() + totalMs;
    while (performance.now() < deadline) {
      var slice = Math.max(200, Math.min(800, deadline - performance.now()));
      try {
        var fr = await this.requestBurst(CMD.GET_STATUS, null, slice, 1);
        var mode = fr.payload.length ? fr.payload[0] : -1;
        if (mode === 5) {
          this.log('OK', '已进入 BOOT（GET_STATUS mode=OTA）');
          return true;
        }
        this.log('WARN', 'GET_STATUS mode=' + mode + '（非 OTA）');
      } catch (e) {
        /* 无应答：App 仍在浅睡或尚未复位完成，继续下一轮 */
      }
    }
    return false;
  };

  SerialOta.prototype.fwBegin = async function (totalLen, crc) {
    var p = new Uint8Array(6);
    p[0] = totalLen & 0xFF; p[1] = (totalLen >>> 8) & 0xFF;
    p[2] = (totalLen >>> 16) & 0xFF; p[3] = (totalLen >>> 24) & 0xFF;
    p[4] = crc & 0xFF; p[5] = (crc >> 8) & 0xFF;
    /* CDC 随机丢 ACK：FW_BEGIN 若 retries=0，首帧丢应答即整段失败。
     * bridge_ota_begin 对重复 begin 会先 abort 再重开，故安全重试。 */
    var fr = await this.request(CMD.FW_BEGIN, p, 1000, 2);
    return fr.payload.length ? fr.payload[0] : 0;
  };

  /* FW_DATA 有效出帧间隔（实测根因，勿调小）：WCH-Link 虚拟串口 CDC 在高速连续
   * 传输下随机丢「整帧 ACK」（插桩 probe：字节完好 crc=0，丢的是应答帧，重发即成功）。
   * 丢帧率与吞吐正相关：gap=5/8ms 时 ~8.5% 且整包中途超时（网页22%、host80% 失败）；
   * gap=20ms 时仅 ~3.4% 重试、178KB 全量稳定跑完。故 FW_DATA 帧间隔下限取 20ms。 */
  SerialOta.prototype.fwData = async function (offset, data) {
    var a = new Uint8Array(4 + data.length);
    a[0] = offset & 0xFF; a[1] = (offset >>> 8) & 0xFF;
    a[2] = (offset >>> 16) & 0xFF; a[3] = (offset >>> 24) & 0xFF;
    a.set(data, 4);
    var need = FW_GAP_MS - (performance.now() - this.lastTx);
    if (need > 0) { await sleep(need); }        /* 实测阈值：≥20ms 稳，<8ms 会中途丢整帧 */
    await this.request(CMD.FW_DATA, a, 2000, 6); /* 重试 6 次，连丢概率 P^7≈2e-6 */
  };

  SerialOta.prototype.fwEnd = async function () {
    await this.request(CMD.FW_END, null, 1500, 0);
  };

  SerialOta.prototype.fwAbort = async function () {
    try { await this.request(CMD.FW_ABORT, null, 1000, 0); } catch (e) { /* ignore */ }
  };

  /** 仅组包（供离线自检 / 日志）：4B 补齐 0xFF + 整包 CRC */
  SerialOta.prepare = function (raw) {
    if (raw.length > MAX_IMAGE) {
      throw new OtaError('镜像 ' + raw.length + 'B 超过上限 216KB');
    }
    var pad = (4 - (raw.length % 4)) % 4;
    var total = raw.length + pad;
    var data = new Uint8Array(total);
    data.set(raw, 0);
    if (pad) { data.fill(0xFF, raw.length); }
    return { raw: raw, data: data, rawLen: raw.length, total: total, crc: global.OtaCrc.crc16(data) };
  };

  /** 整包升级：FW_BEGIN → FW_DATA 循环 → FW_END（SPEC §4.7） */
  SerialOta.prototype.flash = async function (raw, opts) {
    opts = opts || {};
    var chunkOpt = opts.chunk || 0;
    var prepared = SerialOta.prepare(raw);
    this.abortRequested = false;

    /* A 方案：先跳常驻 BOOT 再升级（App 写 UART_IAP 复位，BOOT 驻留收包）。
     * BOOT 无无线栈/睡眠，升级期间不刷 EVT_STATE 心跳；失败/取消回原 App。 */
    await this.enterBoot();

    this.log('INFO', '镜像 原始 ' + prepared.rawLen + 'B → 补齐 ' + prepared.total +
      'B，CRC 0x' + prepared.crc.toString(16).toUpperCase().padStart(4, '0'));

    var chunkMax = await this.fwBegin(prepared.total, prepared.crc);
    if (!chunkMax) { throw new OtaError('FW_BEGIN 被拒绝（chunk_max=0），设备当前不允许升级'); }
    this.log('OK', 'FW_BEGIN 通过，chunk_max=' + chunkMax);

    var n = chunkOpt || Math.min(chunkMax, CHUNK_MAX);
    n -= n % 4;
    if (n <= 0) { n = 4; }
    if (n > chunkMax) { n = chunkMax - (chunkMax % 4); }
    this.log('INFO', '分片 ' + n + 'B/帧，共 ' + Math.ceil(prepared.total / n) + ' 帧');

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
      this.log('OK', 'FW_END 已应答');
    } catch (e) {
      if (e instanceof OtaTimeout || (e instanceof OtaError && e.message.indexOf('断开') >= 0)) {
        /* 预期：设备立即复位搬运，ACK 可能丢失 */
        this.log('WARN', 'FW_END 无应答 —— 设备已复位搬运（视为成功）');
      } else {
        throw e;
      }
    }
    return prepared;
  };

  SerialOta.prototype.abort = function () { this.abortRequested = true; };

  var api = { SerialOta: SerialOta, BAUD: BAUD, CHUNK_MAX: CHUNK_MAX, MAX_IMAGE: MAX_IMAGE };
  global.OtaSerial = api;
  if (typeof module === 'object' && module.exports) { module.exports = api; }
})(typeof window !== 'undefined' ? window : globalThis);
