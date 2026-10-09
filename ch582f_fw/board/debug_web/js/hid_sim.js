/* HID 数据流模拟 —— 文本打字 / 报文构造 / 宏序列 / 压力测试
 *
 * ASCII → HID usage 映射镜像 tools/sk32sim/cli.py 的 _CHAR_MAP（含 Shift 组合）；
 * 打字按「按下 → 释放」两帧交替（规避固件 HID 去重 BRIDGE_ERR_DUP）。
 */
(function (global) {
  'use strict';

  var F = global.DbgFrame || require('./frame.js');

  /* ---------------- ASCII → HID usage（value = [usage, 需 Shift]） ---------------- */
  var CHAR_MAP = {};
  (function () {
    var lower = 'abcdefghijklmnopqrstuvwxyz';
    for (var i = 0; i < 26; i++) {
      CHAR_MAP[lower.charAt(i)] = [0x04 + i, false];
      CHAR_MAP[lower.charAt(i).toUpperCase()] = [0x04 + i, true];
    }
    var digits = '123456789';
    for (var d = 0; d < 9; d++) { CHAR_MAP[digits.charAt(d)] = [0x1E + d, false]; }
    CHAR_MAP['0'] = [0x27, false];
    var shifted = ['!', '@', '#', '$', '%', '^', '&', '*', '(', ')'];
    for (var s = 0; s < 10; s++) { CHAR_MAP[shifted[s]] = [0x1E + s, true]; }
    var named = {
      '\n': [0x28, false], '\r': [0x28, false], '\t': [0x2B, false],
      ' ': [0x2C, false], '\b': [0x2A, false], '\x1b': [0x29, false],
      '-': [0x2D, false], '=': [0x2E, false], '[': [0x2F, false], ']': [0x30, false],
      '\\': [0x31, false], ';': [0x33, false], "'": [0x34, false], '`': [0x35, false],
      ',': [0x36, false], '.': [0x37, false], '/': [0x38, false],
      '_': [0x2D, true], '+': [0x2E, true], '{': [0x2F, true], '}': [0x30, true],
      '|': [0x31, true], ':': [0x33, true], '"': [0x34, true], '~': [0x35, true],
      '<': [0x36, true], '>': [0x37, true], '?': [0x38, true]
    };
    for (var k in named) {
      if (Object.prototype.hasOwnProperty.call(named, k)) { CHAR_MAP[k] = named[k]; }
    }
  })();
  var SHIFT = 0x02;

  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  /* ---------------- 报文构造 ---------------- */
  function keyboardPayload(modifier, keys) {
    var p = new Uint8Array(8);
    p[0] = (modifier || 0) & 0xFF;
    p[1] = 0;
    var k = keys || [];
    for (var i = 0; i < 6 && i < k.length; i++) { p[2 + i] = k[i] & 0xFF; }
    return p;
  }

  /** usage 列表 → NKRO 16B 位图（bit n → usage n+4） */
  function nkroFromUsages(usages) {
    var p = new Uint8Array(16);
    (usages || []).forEach(function (u) {
      var idx = (u | 0) - 4;
      if (idx >= 0 && idx < 128) { p[idx >> 3] |= (1 << (idx & 7)); }
    });
    return p;
  }

  /** 解析文本 → [{ch, usage, shift}]，不支持的字符 usage=null */
  function mapText(text) {
    var out = [];
    for (var i = 0; i < text.length; i++) {
      var ch = text.charAt(i);
      var m = CHAR_MAP[ch];
      out.push(m ? { ch: ch, usage: m[0], shift: m[1] } : { ch: ch, usage: null, shift: false });
    }
    return out;
  }

  /* ---------------- 文本打字 ---------------- */
  /** @param {Commands} cmds @param {string} text
   *  @param {{rateHz?:number, holdMs?:number, onChar?:function, isCancelled?:function}} opts
   *  @returns {Promise<{sent:number, skipped:string[], chars:number}>}
   */
  async function typeText(cmds, text, opts) {
    opts = opts || {};
    var rateHz = Math.max(1, opts.rateHz || 12);
    var holdMs = (opts.holdMs === undefined) ? 15 : opts.holdMs;
    var dtMs = 1000 / rateHz;
    var skipped = [];
    var sent = 0;
    for (var i = 0; i < text.length; i++) {
      if (opts.isCancelled && opts.isCancelled()) { break; }
      var ch = text.charAt(i);
      var m = CHAR_MAP[ch];
      if (!m) {
        skipped.push(ch);
        if (opts.onChar) { opts.onChar(ch, false); }
        continue;
      }
      await cmds.hidKeyboard([m[0]], m[1] ? SHIFT : 0);
      await sleep(Math.max(8, holdMs, dtMs * 0.4));
      await cmds.hidKeyboard([]);
      sent++;
      if (opts.onChar) { opts.onChar(ch, true); }
      await sleep(Math.max(8, dtMs * 0.6));
    }
    return { sent: sent, skipped: skipped, chars: text.length };
  }

  /* ---------------- 宏 / 序列 ---------------- */
  function parseUsage(tok) {
    if (!tok) { return 0; }
    if (/^0x/i.test(tok)) { return parseInt(tok, 16); }
    if (/^[0-9]+$/.test(tok)) { return parseInt(tok, 10); }
    return parseInt(tok, 16);
  }
  function parseDuration(tok) {
    if (!tok) { return 0; }
    var n = parseFloat(tok);
    if (/ms$/i.test(tok)) { return n; }
    if (/s$/i.test(tok)) { return n * 1000; }
    return n;
  }

  /** 行式脚本：
   *    tap 0x04 [holdms]   down 0xE1   up 0xE1   delay 100ms   release
   *  @returns {Promise<{steps:number, lines:number, errors:string[]}>}
   */
  async function runMacro(cmds, script, opts) {
    opts = opts || {};
    var loop = Math.max(1, opts.loop || 1);
    var holdMs = (opts.holdMs === undefined) ? 15 : opts.holdMs;
    var lines = String(script || '').split(/\r?\n/);
    var heldMods = {}, heldKeys = [];
    var steps = 0;
    var errors = [];

    function report() {
      var mod = 0;
      for (var b in heldMods) { if (Object.prototype.hasOwnProperty.call(heldMods, b)) { mod |= (1 << parseInt(b, 10)); } }
      return cmds.hidKeyboard(heldKeys.slice(0, 6), mod);
    }

    for (var lp = 0; lp < loop; lp++) {
      for (var i = 0; i < lines.length; i++) {
        if (opts.isCancelled && opts.isCancelled()) { return { steps: steps, lines: lines.length, errors: errors }; }
        var line = lines[i].trim();
        if (!line || line.charAt(0) === '#') { continue; }
        var tk = line.split(/\s+/);
        var op = tk[0].toLowerCase();
        try {
          if (op === 'tap') {
            var u = parseUsage(tk[1]);
            heldKeys.push(u);
            await report();
            await sleep(tk[2] ? parseDuration(tk[2]) : holdMs);
            heldKeys = heldKeys.filter(function (x) { return x !== u; });
            await report();
          } else if (op === 'down') {
            var ud = parseUsage(tk[1]);
            if (ud >= 0xE0 && ud <= 0xE7) { heldMods[ud - 0xE0] = true; }
            else if (heldKeys.indexOf(ud) < 0) { heldKeys.push(ud); }
            await report();
          } else if (op === 'up') {
            var uu = parseUsage(tk[1]);
            if (uu >= 0xE0 && uu <= 0xE7) { delete heldMods[uu - 0xE0]; }
            else { heldKeys = heldKeys.filter(function (x) { return x !== uu; }); }
            await report();
          } else if (op === 'delay') {
            await sleep(parseDuration(tk[1]));
          } else if (op === 'release') {
            heldKeys = []; heldMods = {};
            await cmds.hidRelease();
          } else {
            errors.push('第 ' + (i + 1) + ' 行：未知指令 "' + op + '"');
          }
          steps++;
        } catch (e) {
          errors.push('第 ' + (i + 1) + ' 行：' + e.message);
        }
      }
    }
    /* 收尾释放，避免卡键 */
    try { await cmds.hidKeyboard([]); } catch (e) { /* ignore */ }
    return { steps: steps, lines: lines.length, errors: errors };
  }

  /* ---------------- 压力测试 ---------------- */
  /** @param {Commands} cmds
   *  @param {{usages?:number[], rateHz?:number, mode?:'time'|'count', seconds?:number,
   *           count?:number, holdMs?:number, onTick?:function, isCancelled?:function}} opts
   *  @returns {Promise<{keys:number, frames:number, errs:number, elapsedMs:number, keysPerSec:number, cancelled:boolean}>}
   */
  async function stress(cmds, opts) {
    opts = opts || {};
    var link = cmds.link;
    var usages = (opts.usages && opts.usages.length) ? opts.usages : [0x04];
    var rateHz = Math.max(1, opts.rateHz || 10);
    var interval = 1000 / rateHz;
    var mode = opts.mode || 'time';
    var seconds = opts.seconds || 5;
    var count = opts.count || 100;
    var holdMs = (opts.holdMs === undefined) ? Math.max(5, Math.min(15, interval / 2)) : opts.holdMs;

    var s0 = link.stats;
    var frames0 = s0.txFrames;
    var errs0 = s0.errorsRx;
    var t0 = (global.performance && global.performance.now) ? global.performance.now() : Date.now();

    var keys = 0, cancelled = false;
    var deadline = t0 + seconds * 1000;
    var idx = 0;
    while (true) {
      if (opts.isCancelled && opts.isCancelled()) { cancelled = true; break; }
      var now = (global.performance && global.performance.now) ? global.performance.now() : Date.now();
      if (mode === 'time') { if (now >= deadline) { break; } }
      else if (keys >= count) { break; }

      var u = usages[idx % usages.length];
      idx++;
      await cmds.hidKeyboard([u], 0);
      await sleep(holdMs);
      await cmds.hidKeyboard([]);
      keys++;
      if (opts.onTick && keys % 10 === 0) { opts.onTick(keys); }
      await sleep(Math.max(2, interval - holdMs));
    }
    var t1 = (global.performance && global.performance.now) ? global.performance.now() : Date.now();
    var elapsed = t1 - t0;
    return {
      keys: keys,
      frames: link.stats.txFrames - frames0,
      errs: link.stats.errorsRx - errs0,
      elapsedMs: elapsed,
      keysPerSec: elapsed > 0 ? (keys / (elapsed / 1000)) : 0,
      cancelled: cancelled
    };
  }

  var api = {
    CHAR_MAP: CHAR_MAP, SHIFT: SHIFT,
    keyboardPayload: keyboardPayload, nkroFromUsages: nkroFromUsages, mapText: mapText,
    typeText: typeText, runMacro: runMacro, stress: stress
  };
  global.DbgHid = api;
  if (typeof module === 'object' && module.exports) { module.exports = api; }
})(typeof window !== 'undefined' ? window : globalThis);
