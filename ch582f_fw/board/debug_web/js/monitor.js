/* 帧监视器 —— 实时帧列表 / 过滤 / 暂停 / 清空 / 导出 / CRC 错误高亮
 *
 * 行结构：时间戳(ms) · 方向(→/←) · 类型(TX/RX/ACK/ERR/EVT/CRC) · CMD 名 · seq · flags ·
 *         payload hex · 解码摘要。点击行展开帧级详情。
 * 性能：仅追加 DOM；过滤通过 dataset 切换 display，避免整表重建（支撑 1000+ 帧）。
 */
(function (global) {
  'use strict';

  var F = global.DbgFrame || require('./frame.js');

  var DEFAULT_MAX = 2000;

  function Monitor(opts) {
    opts = opts || {};
    this.listEl = opts.listEl;
    this.emptyEl = opts.emptyEl || null;
    this.max = opts.max || DEFAULT_MAX;
    this.onCount = opts.onCount || function () {};
    this.rows = [];
    this.paused = false;
    this.filterType = 'ALL';
    this.filterText = '';
  }

  Monitor.prototype.isPaused = function () { return this.paused; };

  Monitor.prototype.togglePause = function () {
    this.paused = !this.paused;
    return this.paused;
  };

  Monitor.prototype.setFilter = function (type, text) {
    this.filterType = type || 'ALL';
    this.filterText = (text || '').toLowerCase();
    this.applyFilter();
  };

  Monitor.prototype.clear = function () {
    this.rows = [];
    if (this.listEl) { this.listEl.innerHTML = ''; }
    this._updateEmpty();
    this.onCount(0);
  };

  /* ---------------- 添加 ---------------- */
  Monitor.prototype.addFrame = function (rec) {
    if (this.paused) { return; }
    var fr = rec.frame;
    var row = {
      dir: rec.dir, t: rec.t, kind: rec.kind,
      cmd: fr ? F.cmdName(fr.cmd) : '',
      seq: fr ? fr.seq : '',
      flags: fr ? fr.flags : 0,
      payloadHex: fr ? F.bytesToHex(fr.payload) : '',
      rawHex: rec.raw ? F.bytesToHex(rec.raw) : '',
      summary: rec.summary || '', crc: false
    };
    this.rows.push(row);
    this._appendRow(row);
    this._trim();
    this.onCount(this.rows.length);
  };

  Monitor.prototype.addCrc = function (raw) {
    if (this.paused) { return; }
    var row = {
      dir: '<-', t: null, kind: 'CRC', cmd: '(CRC 错误)', seq: '', flags: 0,
      payloadHex: '', rawHex: F.bytesToHex(raw), summary: '帧 CRC 校验失败，原始字节', crc: true
    };
    this.rows.push(row);
    this._appendRow(row);
    this._trim();
    this.onCount(this.rows.length);
  };

  Monitor.prototype._appendRow = function (row) {
    if (!this.listEl) { return; }
    var el = document.createElement('div');
    el.className = 'mon-row' + (row.crc ? ' crc' : '') + ' dir-' + (row.dir === 'TX' ? 'tx' : 'rx');
    el.dataset.kind = row.kind;
    el.dataset.text = [row.cmd, row.summary, row.payloadHex, row.rawHex].join(' ').toLowerCase();

    var dir = row.dir === 'TX' ? '→' : '←';
    var parts = [
      ['t', row.t === null ? '--' : (row.t / 1000).toFixed(3) + 's'],
      ['dir', dir],
      ['kind kind-' + row.kind, row.kind],
      ['name', row.cmd],
      ['seq', row.seq === '' ? '' : ('seq=' + row.seq)],
      ['flags', 'fl=0x' + ((row.flags || 0).toString(16).toUpperCase().padStart(2, '0'))],
      ['pay', row.crc ? row.rawHex : row.payloadHex],
      ['sum', row.summary]
    ];
    parts.forEach(function (p) {
      var s = document.createElement('span');
      s.className = 'mon-' + p[0];
      s.textContent = p[1];
      el.appendChild(s);
    });

    /* 详情（点击展开） */
    var det = document.createElement('div');
    det.className = 'mon-detail hidden';
    det.textContent = 'dir=' + row.dir + '  kind=' + row.kind +
      '  cmd=' + row.cmd + '  seq=' + row.seq + '  flags=0x' + (row.flags || 0).toString(16) +
      '\nTX/RX 原始帧: ' + (row.rawHex || '(未记录)') +
      '\npayload: ' + (row.payloadHex || '(空)') +
      '\n摘要: ' + (row.summary || '(无)');
    el.appendChild(det);
    el.addEventListener('click', function () { det.classList.toggle('hidden'); });

    if (!this._match(row)) { el.classList.add('hidden'); }
    this.listEl.appendChild(el);
    this.listEl.scrollTop = this.listEl.scrollHeight;
    this._updateEmpty();
  };

  Monitor.prototype._trim = function () {
    while (this.rows.length > this.max && this.listEl && this.listEl.childElementCount) {
      this.rows.shift();
      this.listEl.removeChild(this.listEl.firstChild);
    }
  };

  Monitor.prototype._match = function (row) {
    if (this.filterType !== 'ALL' && row.kind !== this.filterType) { return false; }
    if (this.filterText) {
      var t = [row.cmd, row.summary, row.payloadHex, row.rawHex].join(' ').toLowerCase();
      if (t.indexOf(this.filterText) < 0) { return false; }
    }
    return true;
  };

  Monitor.prototype.applyFilter = function () {
    if (!this.listEl) { return; }
    var children = this.listEl.children;
    var i;
    var visible = 0;
    /* 第 1 个 child 是 data-row？—— 这里全部为 .mon-row（含内嵌 detail） */
    for (i = 0; i < children.length; i++) {
      var el = children[i];
      if (!el.classList.contains('mon-row')) { continue; }
      var kind = el.dataset.kind;
      var text = el.dataset.text || ''; /* 已含全部字段的小写拼接 */
      var ok = (this.filterType === 'ALL' || kind === this.filterType) &&
        (!this.filterText || text.indexOf(this.filterText) >= 0);
      if (ok) { el.classList.remove('hidden'); visible++; } else { el.classList.add('hidden'); }
    }
    void visible;
    this._updateEmpty();
  };

  Monitor.prototype._updateEmpty = function () {
    if (!this.emptyEl || !this.listEl) { return; }
    var has = this.listEl.childElementCount > 0;
    this.emptyEl.classList.toggle('hidden', has);
  };

  /* ---------------- 导出 ---------------- */
  Monitor.prototype.exportTxt = function () {
    var lines = this.rows.map(function (r) {
      var dir = r.dir === 'TX' ? '→' : (r.dir === '<-' ? '<-' : '←');
      return '[' + (r.t === null ? '--' : (r.t / 1000).toFixed(3) + 's') + '] ' +
        (r.crc ? 'CRC!' : (dir + ' ' + r.kind)) + ' ' + r.cmd +
        (r.seq === '' ? '' : ' seq=' + r.seq) +
        ' flags=0x' + (r.flags || 0).toString(16).toUpperCase() +
        ' ' + (r.payloadHex || r.rawHex) + '  ' + r.summary;
    });
    return lines.join('\n');
  };

  Monitor.prototype.exportJson = function () {
    return JSON.stringify(this.rows.map(function (r) {
      return {
        dir: r.dir === 'TX' ? 'TX' : 'RX', tMs: r.t, kind: r.kind, cmd: r.cmd,
        seq: r.seq, flags: r.flags, payloadHex: r.payloadHex, rawHex: r.rawHex,
        summary: r.summary, crcError: r.crc
      };
    }), null, 2);
  };

  var api = { Monitor: Monitor, DEFAULT_MAX: DEFAULT_MAX };
  global.DbgMonitor = api;
  if (typeof module === 'object' && module.exports) { module.exports = api; }
})(typeof window !== 'undefined' ? window : globalThis);
