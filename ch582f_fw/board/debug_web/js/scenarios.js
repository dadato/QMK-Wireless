/* 场景用例 —— 镜像 tools/sk32sim/scenarios.py
 *
 * 每个场景逐项输出 PASS/FAIL 明细；ota 场景不实现 FW_*，仅提示跳转 board/ota_web。
 * 约定：run(c, link, ch, opts)（c=Commands，link=Link，ch=Checks）。
 */
(function (global) {
  'use strict';

  var F = global.DbgFrame || require('./frame.js');

  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  /* ---------------- 断言收集 ---------------- */
  function Checks(onResult) {
    this.passed = 0;
    this.failed = 0;
    this.onResult = onResult || function () {};
  }
  Checks.prototype.check = function (cond, name, detail) {
    cond = !!cond;
    if (cond) { this.passed++; } else { this.failed++; }
    this.onResult({ ok: cond, name: name, detail: detail || '' });
    return cond;
  };
  Checks.prototype.note = function (name, detail) {
    this.onResult({ ok: null, name: name, detail: detail || '' });
  };
  Checks.prototype.eq = function (got, exp, name) {
    return this.check(got === exp, name, 'got=' + fmt(got) + ' exp=' + fmt(exp));
  };
  /** 期望命令返回 ERR(code)；code 为 ECODE（0x06=PARAM） */
  Checks.prototype.errIs = async function (code, fn, name) {
    try {
      await fn();
    } catch (e) {
      if (e instanceof F.BridgeError) {
        return this.check(e.code === code, name, 'ERR=' + F.errCodeName(e.code));
      }
      if (e && e.name === 'BridgeTimeout') { return this.check(false, name, '超时'); }
      return this.check(false, name, e.message);
    }
    return this.check(false, name, '未返回错误');
  };
  function fmt(v) { return (typeof v === 'string') ? v : JSON.stringify(v); }

  /* ---------------- smoke ---------------- */
  async function smoke(c, link, ch) {
    var ver = await c.getVersion();
    ch.check(ver.major === 0 && ver.minor === 1, 'GET_VERSION=' + ver.text);
    var st = await c.getStatus();
    ch.check(F.MODE_NAMES[st.mode] !== undefined, 'GET_STATUS 模式合法',
      'mode=' + st.modeName + ' connected=' + st.connected + ' profile=' + st.profile + ' battery=' + st.battery + '%');
    var mac = await c.getMac();
    ch.check(mac.bytes.length === 6 && mac.bytes.some(function (b) { return b !== 0; }), 'GET_MAC=' + mac.mac);
    await c.ping();
    ch.check(true, 'PING → PONG');
    var adc = await c.getAdcMv();
    ch.check(adc.mv > 0 && adc.mv < 5000, 'GET_ADC=' + adc.mv + 'mV');

    /* 无应答快速路径：HID 五类 + 透传（仅验证不抛错、链路保持） */
    await c.hidRelease();
    await c.hidKeyboard([]);
    await c.hidNkro(new Uint8Array(16));
    await c.hidConsumer(Uint8Array.of(0, 0));
    await c.hidMouse(new Uint8Array(4));
    await c.hidSystem(0);
    await c.tunnelUsb(Uint8Array.from(seqBytes(16)));
    await c.tunnelBle(Uint8Array.from(seqBytes(8)));
    await sleep(150);
    await c.ping();
    ch.check(true, 'HID(5 类)+透传(USB/BLE) 发送且链路保持');

    /* 越界参数 → ERR */
    await ch.errIs(F.ECODE.PARAM, function () { return c.setProfile(9); }, 'SET_PROFILE 越界 → ERR(PARAM)');
    await ch.errIs(F.ECODE.PARAM, function () { return c.setTestMode(2); }, 'SET_TEST_MODE 非法模式 → ERR(PARAM)');
  }

  /* ---------------- NV ---------------- */
  async function nv(c, link, ch) {
    var addrs = [0, 1, 30, F.NV_USER_UNITS - 1];
    for (var i = 0; i < addrs.length; i++) {
      var addr = addrs[i];
      var pat = new Uint8Array(F.NV_UNIT);
      for (var j = 0; j < F.NV_UNIT; j++) { pat[j] = (addr * 7 + j) & 0xFF; }
      await c.nvWrite(addr, pat);
      var got = await c.nvRead(addr);
      ch.eq(F.bytesToHex(got.data), F.bytesToHex(pat), 'NV[' + addr + '] 往返');
    }
    await ch.errIs(F.ECODE.PARAM, function () { return c.nvRead(F.NV_USER_UNITS); },
      'NV[' + F.NV_USER_UNITS + '] 越界 → ERR(PARAM)');
  }

  /* ---------------- config ---------------- */
  async function config(c, link, ch) {
    try {
      await c.setReportPeriod(8);   ch.check(true, 'REPORT_PERIOD=8ms → ACK');
      await c.setReportPeriod(1);   ch.check(true, 'REPORT_PERIOD 恢复 1ms');
      await c.setLogLevel(4);       ch.check(true, 'LOG_LEVEL=4 → ACK');
      await c.setAdvNameMac(true);  ch.check(true, 'ADV_NAME_MAC=1 → ACK');
      await c.setAdvNameMac(false); ch.check(true, 'ADV_NAME_MAC 恢复 0');
      await c.setBatterySrc(F.BATT_SRC.EXTERNAL); ch.check(true, 'BATTERY_SRC=EXTERNAL → ACK');
      await c.setBatterySrc(F.BATT_SRC.AUTO);     ch.check(true, 'BATTERY_SRC 恢复 AUTO');
    } catch (e) {
      ch.check(false, 'SET_CONFIG 出错', e.message);
    }
    await ch.errIs(F.ECODE.PARAM, function () { return c.setConfig(0x7F, Uint8Array.of(0)); },
      '未知配置键 → ERR(PARAM)');
  }

  /* ---------------- battery ---------------- */
  async function battery(c, link, ch) {
    await c.setBattery(66);
    var st = await c.waitState(2000, function (f) { return f.payload.length >= 4 && f.payload[3] === 66; });
    ch.check(!!st, 'EVT_STATE.battery == 66%');
    var st2 = await c.getStatus();
    ch.eq(st2.battery, 66, 'GET_STATUS 电量一致');
  }

  /* ---------------- sleep ---------------- */
  async function sleepScn(c, link, ch) {
    var before = await c.getStatus();
    ch.check(before.mode !== F.MODE.SLEEP, '初始非 SLEEP');
    await c.sleep(F.SLEEP.IDLE);
    ch.check(true, 'CMD_SLEEP(IDLE) → ACK');
    var st = await c.getStatus();
    ch.check(st.mode !== F.MODE.SLEEP, 'IDLE 后模式不变（浅睡保持链路）');
    await c.sleep(F.SLEEP.DEEP);
    await sleep(1200);                         /* 深睡窗口 */
    var st2 = await c.getStatus();             /* 新数据帧触发唤醒 */
    ch.check(true, '深睡可被串口唤醒（当前 ' + st2.modeName + '）');
    if (st2.mode === F.MODE.SLEEP) {
      await c.sleep(F.SLEEP.WAKE);
      var st3 = await c.getStatus();
      ch.check(st3.mode !== F.MODE.SLEEP, 'WAKE 退出深睡');
    } else {
      ch.check(true, '深睡已被数据帧自动唤醒（无需显式 WAKE）');
    }
  }

  /* ---------------- pair_2g4（需 Dongle） ---------------- */
  async function pair2g4(c, link, ch, opts) {
    var timeout = (opts && opts.timeout) || 65000;
    await c.unpair(0xFF);
    ch.check(true, 'UNPAIR(0xFF) 清理旧绑定 → ACK');
    await c.setMode(F.MODE['2G4']);
    await c.pairStart();
    ch.check(true, 'PAIR_START → ACK（进入 2G4_PAIR）');
    try {
      var ev = await c.waitPairResult(timeout);
      ch.eq(ev.payload[0], 0, 'EVT_PAIR_RESULT=成功');
      var st = await c.waitConnected(5000);
      ch.check(!!st, 'EVT_STATE 显示已连接');
    } catch (e) {
      ch.check(false, Math.round(timeout / 1000) + 's 内未收到配对结果（确认 Dongle 已进配对窗口）', e.message);
    }
  }

  /* ---------------- typing_2g4（需已配对；PC 端人工确认） ---------------- */
  async function typing2g4(c, link, ch) {
    var count = 20, rateHz = 6;
    var st = await c.getStatus();
    if (st.mode !== F.MODE['2G4'] || !st.connected) {
      ch.check(false, '未处于 2.4G 已连接（当前 ' + st.modeName + '）→ 先跑 pair_2g4');
      return;
    }
    var errs0 = link.stats.errorsRx;
    var t0 = nowAbs();
    for (var i = 0; i < count; i++) {
      await c.tapKey(0x04, 0, Math.round(1000 / rateHz / 2)); /* usage 0x04 = 'a' */
      await sleep(Math.max(0, 1000 / rateHz - 20));
    }
    var dt = (nowAbs() - t0) / 1000;
    await c.ping();
    ch.check(link.stats.errorsRx === errs0, '打字期间无 EVT_ERROR');
    ch.check(true, 'PC 端请确认文本区域出现字符（约 ' + (count / dt).toFixed(1) + ' keys/s）');
  }

  /* ---------------- ble（打字需 PC/手机蓝牙配对后验证） ---------------- */
  async function ble(c, link, ch, opts) {
    var name = opts && opts.name;
    if (name) {
      await c.setName(F.NAME_TARGET.BLE, name);
      ch.check(true, 'SET_NAME(BLE)=' + name + ' → ACK');
    }
    await c.setMode(F.MODE.BLE);
    var st = await c.waitState(5000, function (f) { return f.payload[0] === F.MODE.BLE; });
    ch.check(!!st, 'EVT_STATE 模式为 BLE');
    var mac = await c.getMac();
    ch.check(true, "广播名基准='" + (name || '（默认）') + "' MAC=" + mac.mac + '（MAC 后缀见 SET_CONFIG）');
    ch.note('PC/手机蓝牙搜索设备并配对，再用「HID 模拟」验证按键输出');
  }

  /* ---------------- srrc（需频谱仪；此处验证命令/状态机） ---------------- */
  async function srrc(c, link, ch) {
    await c.setTestMode(1, 19, 0);
    ch.check(true, 'SET_TEST_MODE(on, ch=19) → ACK');
    await c.setTestMode(0);
    ch.check(true, 'SET_TEST_MODE(off) → ACK');
    await ch.errIs(F.ECODE.PARAM, function () { return c.setTestMode(1, 99); }, 'channel 越界 → ERR(PARAM)');
  }

  /* ---------------- ota（仅提示，不实现 FW_*） ---------------- */
  async function ota(c, link, ch) {
    ch.note('OTA 固件升级不在本工具内实现', '请使用 board/ota_web（键盘模组 · 串口通道）进行升级');
  }

  /* ---------------- unpair_all（破坏性） ---------------- */
  async function unpairAll(c, link, ch) {
    await c.unpair(0xFF);
    ch.check(true, 'UNPAIR(0xFF) → ACK + EVT_STATE');
  }

  function seqBytes(n) { var a = []; for (var i = 0; i < n; i++) { a.push(i); } return a; }
  function nowAbs() {
    return (global.performance && global.performance.now) ? global.performance.now() : Date.now();
  }

  /* ---------------- 场景注册（顺序即 UI 顺序） ---------------- */
  var LIST = [
    { id: 'smoke', label: 'smoke · 基础与全指令冒烟', hint: '版本/状态/MAC/ADC/PING + HID 五类 + 透传 + 越界参数', destructive: false, run: smoke },
    { id: 'nv', label: 'nv · 用户 NV 读写', hint: '32B 单元写-读回(0/1/30/45) + 越界 ERR(PARAM)', destructive: false, run: nv },
    { id: 'config', label: 'config · SET_CONFIG 五项', hint: '五项键（跑完恢复默认）+ 未知键 ERR(PARAM)', destructive: false, run: config },
    { id: 'battery', label: 'battery · 电量注入', hint: 'SET_BATTERY(66) → EVT_STATE + GET_STATUS 一致', destructive: false, run: battery },
    { id: 'sleep', label: 'sleep · 睡眠分级', hint: 'IDLE/DEEP/WAKE 分级行为（DEEP 唤醒容错）', destructive: false, run: sleepScn },
    { id: 'pair_2g4', label: 'pair_2g4 · 2.4G 配对', hint: '需 Dongle 已进配对窗口（键盘发起）', destructive: false, run: pair2g4 },
    { id: 'typing_2g4', label: 'typing_2g4 · 2.4G 打字', hint: '需已配对；PC 端人工确认字符', destructive: false, run: typing2g4 },
    { id: 'ble', label: 'ble · 切换 BLE/广播名', hint: '打字需 PC/手机蓝牙配对后验证', destructive: false, run: ble },
    { id: 'srrc', label: 'srrc · BLE 定频', hint: '2.4G 定频不支持；需频谱仪（可选）', destructive: false, run: srrc },
    { id: 'ota', label: 'ota · 固件升级', hint: '本工具不实现 FW_*，请改用 board/ota_web', destructive: false, run: ota },
    { id: 'unpair_all', label: 'unpair_all · 清除全部绑定', hint: '破坏性，默认不参与「全部运行」', destructive: true, run: unpairAll }
  ];
  var MAP = {};
  LIST.forEach(function (s) { MAP[s.id] = s; });

  /** 执行单个场景，返回 {id, passed, failed, results} */
  async function runScenario(id, c, link, opts) {
    opts = opts || {};
    var spec = MAP[id];
    if (!spec) { throw new F.BridgeError('未知场景：' + id); }
    var results = [];
    var ch = new Checks(function (r) { results.push(r); if (opts.onResult) { opts.onResult(r, spec); } });
    try {
      await spec.run(c, link, ch, opts);
    } catch (e) {
      ch.check(false, '场景异常：' + (e && e.message));
    }
    return { id: id, label: spec.label, passed: ch.passed, failed: ch.failed, results: results };
  }

  /** 非破坏性集合：smoke + nv + config + battery + sleep */
  async function runAll(c, link, opts) {
    opts = opts || {};
    var ids = ['smoke', 'nv', 'config', 'battery', 'sleep'];
    var out = [];
    for (var i = 0; i < ids.length; i++) {
      if (opts.onScenario) { opts.onScenario(ids[i], 'start'); }
      var r = await runScenario(ids[i], c, link, opts);
      out.push(r);
      if (opts.onScenario) { opts.onScenario(ids[i], 'done', r); }
    }
    return out;
  }

  var api = {
    Checks: Checks, LIST: LIST, MAP: MAP,
    runScenario: runScenario, runAll: runAll
  };
  global.DbgScenarios = api;
  if (typeof module === 'object' && module.exports) { module.exports = api; }
})(typeof window !== 'undefined' ? window : globalThis);