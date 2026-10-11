/* 全命令表 → JS 方法（镜像 tools/sk32sim/client.py）
 *
 * 语义：
 *   - 控制路径（GET_、SET_、PAIR、UNPAIR、NV、BATTERY、SLEEP、ADC、NAME、USB_ID、PING、
 *     ENTER_BOOT 等）：发帧后等待固件 ACK/ERR；失败抛 BridgeError/BridgeTimeout。
 *   - 无应答路径（HID 0x10~0x15 / TUNNEL 0x18/0x19 / RESET）：直接发送不等应答。
 *     注意 ENTER_BOOT(0x08) 属控制路径——先回 ACK，约 60ms 后跳 bootloader（协议 §9.8）。
 *
 * COMMANDS 为命令台 UI 的表驱动元数据（分组 / 字段 / 危险标记 / 执行函数）。
 */
(function (global) {
  'use strict';

  var F = global.DbgFrame || require('./frame.js');
  var D = global.DbgDecode || (typeof require === 'function' ? require('./decode.js') : null);

  function Commands(link) { this.link = link; }

  /* ---------------- 基础 ---------------- */
  Commands.prototype.getVersion = async function () {
    var fr = await this.link.request(F.CMD.GET_VERSION);
    var v = D.decodeVersion(fr.payload);
    return { major: v.major, minor: v.minor, patch: v.patch, text: v.text, frame: fr };
  };
  Commands.prototype.getStatus = async function () {
    var fr = await this.link.request(F.CMD.GET_STATUS);
    var s = D.decodeStatus(fr.payload);
    s.frame = fr;
    return s;
  };
  Commands.prototype.getMode = async function () {
    var fr = await this.link.request(F.CMD.GET_MODE);
    var m = D.decodeMode(fr.payload);
    m.frame = fr;
    return m;
  };
  Commands.prototype.getWirelessState = async function () {
    var fr = await this.link.request(F.CMD.GET_WIRELESS_STATE);
    var w = D.decodeWirelessState(fr.payload);
    w.frame = fr;
    return w;
  };
  Commands.prototype.getBleSlots = async function () {
    var fr = await this.link.request(F.CMD.GET_BLE_SLOTS);
    var s = D.decodeBleSlots(fr.payload);
    s.frame = fr;
    return s;
  };
  Commands.prototype.getMac = async function () {
    var fr = await this.link.request(F.CMD.GET_MAC);
    var m = D.decodeMac(fr.payload);
    m.frame = fr;
    return m;
  };
  Commands.prototype.getAdcMv = async function () {
    var fr = await this.link.request(F.CMD.GET_ADC);
    var a = D.decodeAdc(fr.payload);
    a.frame = fr;
    return a;
  };
  Commands.prototype.ping = function (timeoutMs) {
    return this.link.request(F.CMD.PING, null, timeoutMs || 500, 0);
  };
  Commands.prototype.getDiag = async function () {
    var fr = await this.link.request(F.CMD.GET_DIAG);
    var d = D.decodeDiag(fr.payload);
    d.frame = fr;
    return d;
  };
  Commands.prototype.hidDiag = async function () {
    var fr = await this.link.request(F.CMD.HID_DIAG);
    var d = D.decodeHidDiag(fr.payload);
    d.frame = fr;
    return d;
  };

  /* ---------------- 模式与配对 ---------------- */
  Commands.prototype.setMode = async function (mode, persist) {
    var p = Uint8Array.of(mode & 0xFF, persist === false ? 0 : 1);
    var fr = await this.link.request(F.CMD.SET_MODE, p);
    return { frame: fr, text: '已切换模式 ' + F.modeName(mode) + (persist === false ? '（不落盘）' : '（已落盘）') };
  };
  Commands.prototype.setProfile = async function (profile) {
    var fr = await this.link.request(F.CMD.SET_PROFILE, Uint8Array.of(profile & 0xFF));
    return { frame: fr, text: '已切换 BLE 槽位 ' + profile };
  };
  Commands.prototype.pairStart = async function (duration) {
    if (duration === undefined || duration === null || duration === '') {
      var fr = await this.link.request(F.CMD.PAIR_START);
      return { frame: fr, text: '已进入配对窗口，持续时间用默认值（等待 EVT_PAIR_RESULT）' };
    }
    var d = duration & 0xFFFF;
    var fr = await this.link.request(F.CMD.PAIR_START, Uint8Array.of(d & 0xFF, (d >> 8) & 0xFF));
    return { frame: fr, text: '已进入配对窗口，持续时间 ' + d + 's' + (d === 0 ? '（不自动关闭）' : '') + '（等待 EVT_PAIR_RESULT）' };
  };
  Commands.prototype.unpair = async function (profile) {
    var p = (profile === undefined) ? 0xFF : profile;
    var fr = await this.link.request(F.CMD.UNPAIR, Uint8Array.of(p & 0xFF));
    return { frame: fr, text: (p === 0xFF ? '已清除全部绑定' : ('已解绑槽位 ' + p)) + '（随后 EVT_STATE）' };
  };

  /* ---------------- 配置 ---------------- */
  Commands.prototype.setTestMode = async function (mode, channel, power) {
    var fr = await this.link.request(F.CMD.SET_TEST_MODE,
      Uint8Array.of(mode & 0xFF, (channel || 0) & 0xFF, (power || 0) & 0xFF));
    return { frame: fr, text: '定频设置已下发（2.4G 定频返回 UNSUPPORTED）' };
  };
  Commands.prototype.setConfig = async function (key, value) {
    var v = value || new Uint8Array(0);
    var p = new Uint8Array(1 + v.length);
    p[0] = key & 0xFF;
    p.set(v, 1);
    var fr = await this.link.request(F.CMD.SET_CONFIG, p);
    return { frame: fr, text: '配置已写入 ' + (F.CFG_KEY_NAMES[key] || ('0x' + key.toString(16))) };
  };
  Commands.prototype.setReportPeriod = function (ms) {
    return this.setConfig(F.CFG_KEY.REPORT_PERIOD, Uint8Array.of(ms & 0xFF, (ms >> 8) & 0xFF));
  };
  Commands.prototype.setLogLevel = function (level) {
    return this.setConfig(F.CFG_KEY.LOG_LEVEL, Uint8Array.of(level & 0xFF));
  };
  Commands.prototype.setAdvNameMac = function (en) {
    return this.setConfig(F.CFG_KEY.ADV_NAME_MAC, Uint8Array.of(en ? 1 : 0));
  };
  Commands.prototype.setConnInterval = function (units) {
    return this.setConfig(F.CFG_KEY.CONN_INTERVAL, Uint8Array.of(units & 0xFF));
  };
  Commands.prototype.setBatterySrc = function (src) {
    return this.setConfig(F.CFG_KEY.BATTERY_SRC, Uint8Array.of(src & 0xFF));
  };
  Commands.prototype.setSleepPolicy = function (policy) {
    return this.setConfig(F.CFG_KEY.SLEEP_POLICY, Uint8Array.of(policy & 0xFF));
  };
  Commands.prototype.setHeartbeat = async function (enable) {
    var fr = await this.link.request(F.CMD.SET_HEARTBEAT, Uint8Array.of(enable ? 1 : 0));
    return { frame: fr, text: '心跳周期已' + (enable ? '开启' : '关闭') + '（关闭后不发 PING/不做失联判定）' };
  };

  /* ---------------- 电源与其他 ---------------- */
  Commands.prototype.setBattery = async function (percent) {
    var fr = await this.link.request(F.CMD.SET_BATTERY, Uint8Array.of(percent & 0xFF));
    return { frame: fr, text: '已注入电量 ' + percent + '%（随后 EVT_STATE）' };
  };
  Commands.prototype.sleep = async function (level) {
    var fr = await this.link.request(F.CMD.SLEEP, Uint8Array.of(level & 0xFF));
    var names = { 0: '唤醒', 1: '浅睡', 2: '深睡' };
    return { frame: fr, text: '睡眠分级 ' + (names[level] || level) + ' 已下发' };
  };
  Commands.prototype.sleepCfg = async function (idleS, deepS) {
    var i = idleS & 0xFFFF, d = deepS & 0xFFFF;
    var fr = await this.link.request(F.CMD.SLEEP_CFG,
      Uint8Array.of(i & 0xFF, (i >> 8) & 0xFF, d & 0xFF, (d >> 8) & 0xFF));
    return { frame: fr, text: '睡眠时间已配置 IDLE=' + i + 's / DEEP=' + d + 's（0=禁用该项自动休眠，仅 AUTO 策略生效）' };
  };
  Commands.prototype.nvWrite = async function (addr, data) {
    if (data.length !== F.NV_UNIT) {
      throw new F.BridgeError('NV 单元必须 ' + F.NV_UNIT + 'B，当前 ' + data.length + 'B');
    }
    var p = new Uint8Array(1 + F.NV_UNIT);
    p[0] = addr & 0xFF;
    p.set(data, 1);
    var fr = await this.link.request(F.CMD.NV_WRITE, p);
    return { frame: fr, text: 'NV[' + addr + '] 已写入 ' + F.NV_UNIT + 'B' };
  };
  Commands.prototype.nvRead = async function (addr) {
    var fr = await this.link.request(F.CMD.NV_READ, Uint8Array.of(addr & 0xFF));
    var r = D.decodeNvRead(fr.payload);
    r.frame = fr;
    return r;
  };
  Commands.prototype.setName = async function (target, name) {
    var bytes = (typeof name === 'string')
      ? asciiBytes(name) : new Uint8Array(name);
    var p = new Uint8Array(1 + bytes.length);
    p[0] = target & 0xFF;
    p.set(bytes, 1);
    var fr = await this.link.request(F.CMD.SET_NAME, p);
    return { frame: fr, text: '名称已设置（target=' + (target === F.NAME_TARGET.BLE ? 'BLE' : 'USB') + '）' };
  };
  Commands.prototype.setUsbId = async function (vid, pid) {
    var p = Uint8Array.of(vid & 0xFF, (vid >> 8) & 0xFF, pid & 0xFF, (pid >> 8) & 0xFF);
    var fr = await this.link.request(F.CMD.SET_USB_ID, p);
    return { frame: fr, text: 'USB 标识已改写（Dongle 侧重枚举；键盘侧同步写 BLE DIS PnP ID）' };
  };

  /* ---------------- 危险操作 ---------------- */
  Commands.prototype.reset = function () {
    return this.link.sendFast(F.CMD.RESET, new Uint8Array(0))
      .then(function () { return { text: '已发送复位（无应答，设备随即重启）' }; });
  };
  Commands.prototype.enterBoot = async function () {
    /* 协议 §9.8：先回 ACK，约 60ms 后释放 HID 并跳 bootloader */
    var fr = await this.link.request(F.CMD.ENTER_BOOT, new Uint8Array(0));
    return { frame: fr, text: '已进入 ROM ISP（ACK 后约 60ms 跳 bootloader）' };
  };

  /* ---------------- HID 快速路径 ---------------- */
  Commands.prototype.hidKeyboard = function (keys, modifier) {
    var k = keys || [];
    var payload = new Uint8Array(8);
    payload[0] = (modifier || 0) & 0xFF;
    payload[1] = 0;
    for (var i = 0; i < 6 && i < k.length; i++) { payload[2 + i] = k[i] & 0xFF; }
    return this.link.sendFast(F.CMD.HID_KEYBOARD, payload);
  };
  Commands.prototype.tapKey = async function (usage, modifier, holdMs) {
    await this.hidKeyboard([usage], modifier || 0);
    await sleep(holdMs === undefined ? 15 : holdMs);
    await this.hidKeyboard([]);
  };
  Commands.prototype.hidNkro = function (bitmap) {
    var b = bitmap || new Uint8Array(0);
    var payload = new Uint8Array(16);
    for (var i = 0; i < 16 && i < b.length; i++) { payload[i] = b[i] & 0xFF; }
    return this.link.sendFast(F.CMD.HID_NKRO, payload);
  };
  Commands.prototype.hidConsumer = function (usage) {
    var u = usage || new Uint8Array(0);
    var payload = new Uint8Array(2);
    payload[0] = u[0] || 0;
    payload[1] = u[1] || 0;
    return this.link.sendFast(F.CMD.HID_CONSUMER, payload);
  };
  Commands.prototype.hidMouse = function (report) {
    var r = report || new Uint8Array(0);
    var payload = new Uint8Array(4);
    for (var i = 0; i < 4 && i < r.length; i++) { payload[i] = r[i] & 0xFF; }
    return this.link.sendFast(F.CMD.HID_MOUSE, payload);
  };
  Commands.prototype.hidSystem = function (code) {
    return this.link.sendFast(F.CMD.HID_SYSTEM, Uint8Array.of(code & 0xFF));
  };
  Commands.prototype.hidRelease = function () {
    return this.link.sendFast(F.CMD.HID_RELEASE, new Uint8Array(0));
  };

  /* ---------------- 透传 ---------------- */
  Commands.prototype.tunnelUsb = function (data) {
    var d = data || new Uint8Array(0);
    return this.link.sendFast(F.CMD.TUNNEL_USB, d.subarray(0, F.TUNNEL_USB_LEN));
  };
  Commands.prototype.tunnelBle = function (data) {
    var d = data || new Uint8Array(0);
    return this.link.sendFast(F.CMD.TUNNEL_BLE, d.subarray(0, F.TUNNEL_BLE_LEN));
  };

  /* ---------------- 事件等待 ---------------- */
  Commands.prototype.waitState = function (timeoutMs, pred) {
    return this.link.waitEvent(timeoutMs || 5000, F.EVT.STATE, pred);
  };
  Commands.prototype.waitPairResult = function (timeoutMs) {
    return this.link.waitEvent(timeoutMs || 65000, F.EVT.PAIR_RESULT);
  };
  /* 等待主机电源状态事件（EVT_HOST_POWER）：pred([state]) 可选，默认匹配任意状态 */
  Commands.prototype.waitHostPower = function (timeoutMs, pred) {
    var p = pred;
    if (typeof pred === 'number') { p = function (f) { return f.payload[0] === pred; }; }
    return this.link.waitEvent(timeoutMs || 5000, F.EVT.HOST_POWER, p);
  };
  Commands.prototype.waitConnected = function (timeoutMs) {
    return this.waitState(timeoutMs || 30000, function (f) {
      return f.payload.length >= 2 && f.payload[1] === 1;
    });
  };

  /* ---------------- 工具 ---------------- */
  function asciiBytes(s) {
    var out = [];
    for (var i = 0; i < s.length; i++) {
      var c = s.charCodeAt(i);
      if (c < 128) { out.push(c); }
    }
    return Uint8Array.from(out);
  }
  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  /* =====================================================================
   * 命令台元数据（表驱动）：字段类型 number/text/hex/select/checkbox
   * run(cmds, vals) → Promise<{text, frame?}>
   * =================================================================== */
  var COMMANDS = [
    {
      name: '基础', items: [
        { id: 'get_version', label: 'GET_VERSION', cmd: F.CMD.GET_VERSION,
          desc: '读取固件版本号。\n请求：无 payload。\n应答：ACK + 3B [major][minor][patch]（当前 0.1.0）。\n错误码：无。',
          fields: [],
          run: async function (c) { var v = await c.getVersion(); return { text: '版本 ' + v.text, frame: v.frame }; } },
        { id: 'get_status', label: 'GET_STATUS', cmd: F.CMD.GET_STATUS,
          desc: '一次读取模式/连接/槽位/电量/休眠策略。\n请求：无 payload。\n应答：ACK + 5B [mode][connected][profile][battery%][sleep_policy]。\n  [0]mode：0=USB / 1=BLE / 2=BLE_PAIR / 3=2G4 / 4=2G4_PAIR / 5=OTA / 6=SLEEP。\n  [1]connected：0=未连接 / 1=无线已连接。\n  [2]profile：当前 BLE 设备槽 1..4。\n  [3]battery%：电量 0..100。\n  [4]sleep_policy：0=AUTO（自动）/ 1=MANUAL（手动）。\n错误码：无。',
          fields: [],
          run: async function (c) { var s = await c.getStatus(); return { text: '模式=' + s.modeName + ' 连接=' + (s.connected ? '是' : '否') + ' 槽位=' + s.profile + ' 电量=' + s.battery + '% 休眠策略=' + (s.sleepPolicy ? 'MANUAL' : 'AUTO'), frame: s.frame }; } },
        { id: 'get_mode', label: 'GET_MODE', cmd: F.CMD.GET_MODE,
          desc: '轻量模式查询（高频轮询用，不含槽位/电量）。\n请求：无 payload。\n应答：ACK + 3B [mode][connected][radio]。\n  [0]mode：0=USB / 1=BLE / 2=BLE_PAIR / 3=2G4 / 4=2G4_PAIR / 5=OTA / 6=SLEEP。\n  [1]connected：0=未连接 / 1=无线已连接。\n  [2]radio：0=NONE / 1=BLE / 2=2G4。\n错误码：无。',
          fields: [],
          run: async function (c) { var m = await c.getMode(); return { text: '模式=' + m.modeName + ' 连接=' + (m.connected ? '是' : '否') + ' 射频=' + m.radioName, frame: m.frame }; } },
        { id: 'get_wireless_state', label: 'GET_WIRELESS_STATE', cmd: F.CMD.GET_WIRELESS_STATE,
          desc: '读取无线综合状态（BRIDGE_WL_*）。\n请求：无 payload。\n应答：ACK + 1B [state]：0=USB/1=IDLE/2=BLE_PAIRING/3=2G4_PAIRING/4=BLE_LINKED/5=2G4_LINKED/6=BLE_NOLINK/7=2G4_NOLINK/8=OTA/9=SLEEP。\n错误码：无。',
          fields: [],
          run: async function (c) { var w = await c.getWirelessState(); return { text: '无线状态=' + w.name + '（' + w.state + '）', frame: w.frame }; } },
        { id: 'get_ble_slots', label: 'GET_BLE_SLOTS', cmd: F.CMD.GET_BLE_SLOTS,
          desc: '查询 BLE 多主机槽（4 组 Profile）绑定状态。\n请求：无 payload。\n应答：ACK + 5B [s1][s2][s3][s4][last_active]。\n  [0..3]s1..s4：槽 1..4 是否已绑定（0=空 / 1=已绑定）。\n  [4]last_active：最近活跃槽 1..4，0=无。\n错误码：仅键盘模组支持；Dongle 侧返回 ERR STATE(0x07)（UNSUPPORTED）。',
          fields: [],
          run: async function (c) { var s = await c.getBleSlots(); return { text: D.bleSlotsText(s), frame: s.frame }; } },
        { id: 'get_mac', label: 'GET_MAC', cmd: F.CMD.GET_MAC,
          desc: '读取本机 6 字节蓝牙 MAC。\n请求：无 payload。\n应答：ACK + 6B [b0..b5]（LE 排列）。\n错误码：底层读取失败 → STATE。',
          fields: [],
          run: async function (c) { var m = await c.getMac(); return { text: 'MAC ' + m.mac, frame: m.frame }; } },
        { id: 'get_adc', label: 'GET_ADC', cmd: F.CMD.GET_ADC,
          desc: '读取电池 ADC 电压。\n请求：无 payload。\n应答：ACK + 2B [mv_lo][mv_hi]（LE，单位 mV）。\n错误码：无。',
          fields: [],
          run: async function (c) { var a = await c.getAdcMv(); return { text: 'ADC ' + a.mv + ' mV', frame: a.frame }; } },
        { id: 'get_diag', label: 'GET_DIAG', cmd: F.CMD.GET_DIAG,
          desc: '读取 2.4G 射频诊断计数（24B，排障用）。\n请求：无 payload。\n应答：ACK + 24B：[0]up [1]conn [2]bound [3]tx_busy [4]tx_q_cnt [5]hb_miss [6..7]report_period，[8..23] 依次为 tx_submit_ok/fail、tx_done_ok/fail、tx_timeout、rx_any、hop_shut、rx_data_bad（[6..23] 均 LE16）。\n错误码：无。',
          fields: [],
          run: async function (c) { var d = await c.getDiag(); return { text: D.diagText(d), frame: d.frame }; } },
        { id: 'hid_diag', label: 'HID_DIAG', cmd: F.CMD.HID_DIAG,
          desc: '读取 BLE HID 服务注册取证（26B，排障用）。\n请求：无 payload。\n应答：ACK + 26B：[0]reg（GATTServApp_RegisterService 返回值，0=成功/0xFF=未调用）[1..2]hid_handle [3..4]map_handle [5..6]raw_in_handle(RID5 IN) [7..8]raw_out_handle(RID5 OUT)（[1..8] 均 LE16）[9]report_cnt [10]attr_cnt [11]reset_src(R8_RESET_STATUS&0x07) [12]stage_prev [13]stage_now；[14..17]/[18..21]/[22..25] 为 3 个 uint32(LE) 出站 notify 计数：fail(HidDev_Report 失败)/retry_ok(延迟重发成功)/retry_drop(延迟重发仍失败)。\n错误码：仅键盘模组支持；Dongle 侧返回 ERR STATE(0x07)。',
          fields: [],
          run: async function (c) { var d = await c.hidDiag(); return { text: D.hidDiagText(d), frame: d.frame }; } },
        { id: 'ping', label: 'PING', cmd: F.CMD.PING,
          desc: '链路心跳/对端探测。\n请求：无 payload。\n应答：心跳开 → EVT_PONG（FLAG_ACK，seq 回环）；心跳关 → 直接 ACK 不回 PONG。桥每 1000ms 主动发 PING。\n错误码：ACK 超时。',
          fields: [],
          run: async function (c) { var fr = await c.ping(); return { text: 'PONG（链路正常）', frame: fr }; } }
      ]
    },
    {
      name: '模式与配对', items: [
        { id: 'set_mode', label: 'SET_MODE', cmd: F.CMD.SET_MODE,
          desc: '切换工作模式（完整状态迁移，会主动下发 EVT_STATE）。\n请求：[mode]（+[persist]，缺省 1=落盘）。\nmode 取值：\n  0 = USB（USB 直连；无 VBUS 时允许切换但不落盘）\n  1 = BLE（BLE 常规：已连接/待连）\n  2 = BLE_PAIR（BLE 配对窗口；禁用 → PARAM，请改用 PAIR_START）\n  3 = 2G4（2.4G 常规）\n  4 = 2G4_PAIR（2.4G 配对窗口；禁用 → PARAM，请改用 PAIR_START）\n  5 = OTA（固件升级中；不落盘）\n  6 = SLEEP（休眠；不落盘）\n  >6 → PARAM\npersist：0=不落盘 / 1=落盘（缺省 1）。\n边界：mode=2/4 → PARAM；>6 → PARAM；有 VBUS 且 mode≠USB → STATE；SLEEP/OTA 不落盘。\n错误码：PARAM / STATE（VBUS 冲突）。',
          fields: [
          { key: 'mode', label: '模式', type: 'select', def: '0',
            options: [['0', '0 = USB'], ['1', '1 = BLE'], ['3', '3 = 2G4'], ['5', '5 = OTA'], ['6', '6 = SLEEP']] },
          { key: 'persist', label: '落盘', type: 'checkbox', def: true }
        ], run: function (c, v) { return c.setMode(parseInt(v.mode, 10), v.persist); } },
        { id: 'set_profile', label: 'SET_PROFILE', cmd: F.CMD.SET_PROFILE,
          desc: '切换 BLE 多设备槽位（1..4）；成功后释放全部 HID 并下发 EVT_STATE。\n请求：[profile]（0~1B）；无 payload 缺省 1。\nprofile 取值：1=设备槽1 / 2=设备槽2 / 3=设备槽3 / 4=设备槽4；0 或 >4 → PARAM。\n错误码：PARAM。',
          fields: [
          { key: 'profile', label: '槽位', type: 'number', def: '1', min: 1, max: 4 }
        ], run: function (c, v) { return c.setProfile(parseInt(v.profile, 10)); } },
        { id: 'pair_start', label: 'PAIR_START', cmd: F.CMD.PAIR_START,
          desc: '进入配对窗口，按当前射频方向配对。\n请求：[duration_s LE16]（可选，单位 s）。\n  duration_s：窗口持续时间秒数；缺省=编译期默认 60s；0=不自动关闭（无限等待，需手动 SET_MODE 退出）；>0=超时自动回落。\n现象：窗口超时 → EVT_PAIR_RESULT(fail) + 回落（BLE_PAIR→BLE / 2G4_PAIR→2G4）；成功 → EVT_PAIR_RESULT(ok)。\n错误码：有 VBUS → STATE；payload 非 0/2 字节 → PARAM；其他失败 → PAIR。',
          fields: [
          { key: 'duration_s', label: '持续时间s(空=默认,0=不关闭)', type: 'number', def: '', min: 0, max: 65535 }
        ], run: function (c, v) { var s = v.duration_s; return c.pairStart(s === '' || s === undefined ? undefined : parseInt(s, 10)); } },
        { id: 'unpair', label: 'UNPAIR', cmd: F.CMD.UNPAIR, dangerous: true,
          desc: '解绑已配对设备（先回 ACK，再下发 EVT_STATE）。\n请求：[profile]（0~1B，破坏性）。\nprofile 取值：1..4=仅清除指定设备槽；0xFF=清除全部绑定（1..4）；无 payload 缺省 0xFF。\n错误码：无专用。',
          confirm: 'UNPAIR 将解除绑定；0xFF 会清除全部配对。确认发送？', fields: [
            { key: 'profile', label: '槽位(255=全清)', type: 'number', def: '255', min: 0, max: 255 }
          ], run: function (c, v) { return c.unpair(parseInt(v.profile, 10)); } }
      ]
    },
    {
      name: '配置', items: [
        { id: 'report_period', label: 'SET_CONFIG · REPORT_PERIOD', cmd: F.CMD.SET_CONFIG,
          desc: '写配置 key=0x01：2.4G 上报周期。\n请求：value=LE16（ms），缺省 1。\n值域：1..100（ms）；0 或 >100 → PARAM。\n错误码：PARAM。',
          fields: [
          { key: 'ms', label: 'ms', type: 'number', def: '8', min: 1, max: 100 }
        ], run: function (c, v) { return c.setReportPeriod(parseInt(v.ms, 10)); } },
        { id: 'log_level', label: 'SET_CONFIG · LOG_LEVEL', cmd: F.CMD.SET_CONFIG,
          desc: '写配置 key=0x02：日志等级。\n请求：value=1B 枚举。\nlevel 取值：0=OFF / 1=ERROR / 2=WARN / 3=INFO / 4=DEBUG；>4 → PARAM。\n注意：BRIDGE_LOG_ENABLE=0 时恒 UNSUPPORTED（对外报 PARAM）。\n错误码：PARAM。',
          fields: [
          { key: 'level', label: '级别', type: 'select', def: '4',
            options: [['0', '0 = OFF'], ['1', '1 = ERROR'], ['2', '2 = WARN'], ['3', '3 = INFO'], ['4', '4 = DEBUG']] }
        ], run: function (c, v) { return c.setLogLevel(parseInt(v.level, 10)); } },
        { id: 'adv_name_mac', label: 'SET_CONFIG · ADV_NAME_MAC', cmd: F.CMD.SET_CONFIG,
          desc: '写配置 key=0x03：广播名是否附 MAC 后缀。\n请求：value=1B。\n取值：0=否 / 1=是；缺省 1。\n错误码：PARAM。',
          fields: [
          { key: 'en', label: '启用', type: 'select', def: '1', options: [['1', '1 = 是'], ['0', '0 = 否']] }
        ], run: function (c, v) { return c.setAdvNameMac(parseInt(v.en, 10) !== 0); } },
        { id: 'conn_interval', label: 'SET_CONFIG · CONN_INTERVAL', cmd: F.CMD.SET_CONFIG,
          desc: '写配置 key=0x04：BLE 连接间隔。\n请求：value=1B（直接存，1 单位 ≈ 1.25ms）。\n取值：0..255；6 ≈ 7.5ms；缺省 6。\n错误码：PARAM。',
          fields: [
          { key: 'units', label: '单位', type: 'number', def: '6', min: 0, max: 255 }
        ], run: function (c, v) { return c.setConnInterval(parseInt(v.units, 10)); } },
        { id: 'battery_src', label: 'SET_CONFIG · BATTERY_SRC', cmd: F.CMD.SET_CONFIG,
          desc: '写配置 key=0x05：电量来源。\n请求：value=1B。\n取值：0=AUTO（自动）/ 1=LOCAL（本地 ADC）/ 2=EXTERNAL（外部上报）；缺省 0。\n错误码：PARAM。',
          fields: [
          { key: 'src', label: '来源', type: 'select', def: '0', options: [['0', '0 = AUTO'], ['1', '1 = LOCAL'], ['2', '2 = EXTERNAL']] }
        ], run: function (c, v) { return c.setBatterySrc(parseInt(v.src, 10)); } },
        { id: 'sleep_policy', label: 'SET_CONFIG · SLEEP_POLICY', cmd: F.CMD.SET_CONFIG,
          desc: '写配置 key=0x06：休眠策略。\n请求：value=1B。\n取值：0=AUTO（自动判闲休眠）/ 1=MANUAL（仅 SLEEP 指令）；缺省 0。\n错误码：PARAM。',
          fields: [
          { key: 'policy', label: '策略', type: 'select', def: '0', options: [['0', '0 = AUTO（自动休眠）'], ['1', '1 = MANUAL（仅指令）']] }
        ], run: function (c, v) { return c.setSleepPolicy(parseInt(v.policy, 10)); } },
        { id: 'set_heartbeat', label: 'SET_HEARTBEAT', cmd: F.CMD.SET_HEARTBEAT,
          desc: '启用/关闭 PING 心跳。\n请求：[enable] 1B。\n取值：0=关闭 / 1=开启；缺省 1（开启）。\n现象：关闭后不做失联判定，PING 仅回 ACK 不回 PONG。\n边界：len<1 或 payload[0]>1 → PARAM。\n错误码：PARAM。',
          fields: [
          { key: 'enable', label: '状态', type: 'select', def: '1', options: [['1', '1 = 开启'], ['0', '0 = 关闭']] }
        ], run: function (c, v) { return c.setHeartbeat(parseInt(v.enable, 10) !== 0); } },
        { id: 'config_raw', label: 'SET_CONFIG · 原始键', cmd: F.CMD.SET_CONFIG,
          desc: '写任意配置键（调试用）。\n请求：[key][value...]，长度须与 key 匹配。\n键表：0x01=REPORT_PERIOD(2B) / 0x02=LOG_LEVEL(1B) / 0x03=ADV_NAME_MAC(1B) / 0x04=CONN_INTERVAL(1B) / 0x05=BATTERY_SRC(1B) / 0x06=SLEEP_POLICY(1B)；其他 → UNSUPPORTED（对外报 PARAM）。\n边界：key 不识别 / 长度不匹配 / 值非法 → PARAM。',
          fields: [
          { key: 'key', label: 'key(hex)', type: 'hex', def: '7F', maxBytes: 1 },
          { key: 'value', label: 'value(hex)', type: 'hex', def: '' }
        ], run: function (c, v) {
          var key = F.hexToBytes(v.key)[0] || 0;
          return c.setConfig(key, F.hexToBytes(v.value));
        } }
      ]
    },
    {
      name: '电源与其他', items: [
        { id: 'set_battery', label: 'SET_BATTERY', cmd: F.CMD.SET_BATTERY,
          desc: '外部上报电量百分比（先回 ACK，再下发 EVT_STATE）。\n请求：[percent]（1B）。\npercent 取值：0x00..0x64 = 0..100%；>0x64 截断为 100%。\n现象：仅 battery_src=EXTERNAL 时优先于本地 ADC；低电告警 LOW ≤20% / CRIT ≤5%。\n边界：len<1 → PARAM。',
          fields: [
          { key: 'percent', label: '电量% (0..100)', type: 'number', def: '66', min: 0, max: 100 }
        ], run: function (c, v) { return c.setBattery(parseInt(v.percent, 10)); } },
        { id: 'sleep', label: 'SLEEP', cmd: F.CMD.SLEEP,
          desc: '睡眠控制 / 唤醒。\n请求：[level]（0~1B，缺省 1=IDLE）。\nlevel 取值：\n  0 = WAKE（解除睡眠许可，切回 last_mode；上次为 SLEEP/OTA 则改 BLE）\n  1 = IDLE（浅睡，保持链路；缺省）\n  2 = DEEP（进 MODE_SLEEP）\n  >2 → PARAM\n边界：>2 → PARAM。',
          fields: [
          { key: 'level', label: '分级', type: 'select', def: '1', options: [['0', '0 = WAKE'], ['1', '1 = IDLE（缺省）'], ['2', '2 = DEEP']] }
        ], run: function (c, v) { return c.sleep(parseInt(v.level, 10)); } },
        { id: 'sleep_cfg', label: 'SLEEP_CFG', cmd: F.CMD.SLEEP_CFG,
          desc: '配置自动休眠（AUTO 策略）的时间门限，落盘保存。\n请求：[idle_s LE16][deep_s LE16]（各 2B，单位 s）。\n  idle_s：距最近一次串口接收活动达该秒数后允许浅睡（IDLE，保持链路）；0=禁用自动浅睡。\n  deep_s：未连接且空闲达该秒数后进深睡（DEEP，断连）；0=禁用自动深睡。\n边界：len≠4 → PARAM；仅 sleep_policy=AUTO 时生效（MANUAL 仅响应 SLEEP 指令）。\n默认：idle_s=1 / deep_s=30。',
          fields: [
          { key: 'idle_s', label: 'IDLE 门限s (0=禁用)', type: 'number', def: '1', min: 0, max: 65535 },
          { key: 'deep_s', label: 'DEEP 门限s (0=禁用)', type: 'number', def: '30', min: 0, max: 65535 }
        ], run: function (c, v) { return c.sleepCfg(parseInt(v.idle_s, 10), parseInt(v.deep_s, 10)); } },
        { id: 'nv_read', label: 'NV_READ', cmd: F.CMD.NV_READ,
          desc: '读用户 NV 单元。\n请求：[addr]（1B）。\naddr 取值：0..45（0x00..0x2D），共 46 个单元 × 32B。\n应答：ACK + 33B [addr][data0..31]。\n边界：len<1 或 addr≥46 → PARAM；底层读失败 → STATE。',
          fields: [
          { key: 'addr', label: '单元 (0..45)', type: 'number', def: '0', min: 0, max: 45 }
        ], run: async function (c, v) { var r = await c.nvRead(parseInt(v.addr, 10)); return { text: 'NV[' + r.addr + '] ' + F.bytesToHex(r.data), frame: r.frame }; } },
        { id: 'nv_write', label: 'NV_WRITE', cmd: F.CMD.NV_WRITE,
          desc: '写 32B 用户 NV 单元（读改写整槽）。\n请求：33B [addr][data32]。\naddr 取值：0..45（0x00..0x2D），共 46 个单元。\n边界：len≠33 或 addr≥46 → PARAM。',
          fields: [
          { key: 'addr', label: '单元 (0..45)', type: 'number', def: '0', min: 0, max: 45 },
          { key: 'data', label: '数据(32B hex)', type: 'hex', def: '00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF', maxBytes: 32 }
        ], run: function (c, v) { return c.nvWrite(parseInt(v.addr, 10), F.hexToBytes(v.data)); } },
        { id: 'set_name', label: 'SET_NAME', cmd: F.CMD.SET_NAME,
          desc: '设置 BLE 广播名 / USB 设备名。\n请求：[target][name...]。\ntarget 取值：0 = BLE（广播名）/ 1 = USB（设备名）。\nname：≤21 字节（BRIDGE_NAME_MAX，无 NUL 结尾，仅按长度写入）。\n现象：本地不支持（如 Dongle 侧）经 2.4G 中继到对端；失败 → STATE。\n边界：len<2 / target∉{0,1} / len-1>21 → PARAM。\n缺省名：BLE "SK32 Keyboard"、USB "SK32 Wireless"。',
          fields: [
          { key: 'target', label: '目标', type: 'select', def: '0', options: [['0', '0 = BLE'], ['1', '1 = USB']] },
          { key: 'name', label: '名称(≤21B)', type: 'text', def: 'SK32-KB', maxlength: 21 }
        ], run: function (c, v) { return c.setName(parseInt(v.target, 10), v.name); } },
        { id: 'set_usb_id', label: 'SET_USB_ID', cmd: F.CMD.SET_USB_ID,
          desc: '设置 USB VID/PID。\nDongle 侧：改写 USB 描述符并重枚举；键盘模组侧：本地同步改写 BLE DIS PnP ID' +
                '（主机 BLE 读到的 VID/PID，需移除设备后重新配对才刷新），并回 UNSUPPORTED 由 core 经 2.4G 中继到 Dongle。\n' +
                '请求：必须 4B [vid_lo][vid_hi][pid_lo][pid_hi]（均 LE16）。\n' +
                '边界：len≠4 或 vid==0 或 pid==0 → PARAM。\n' +
                'LD7 键盘标识：VID=0x0194 / PID=0x0464（本地按 LE 打包上送）。',
          fields: [
          { key: 'vid', label: 'VID(hex)', type: 'hex', def: '0194', maxBytes: 2 },
          { key: 'pid', label: 'PID(hex)', type: 'hex', def: '0464', maxBytes: 2 }
        ], run: function (c, v) {
          return c.setUsbId(parseInt(v.vid, 16) & 0xFFFF, parseInt(v.pid, 16) & 0xFFFF);
        } },
        { id: 'set_test_mode', label: 'SET_TEST_MODE', cmd: F.CMD.SET_TEST_MODE,
          desc: 'SRRC 定频发射测试（编译开关 BRIDGE_FEATURE_SRRC_TEST 已开）。\n请求：[mode][channel][power]。\nmode 取值：0 = 退出（未激活时幂等 OK）/ 1 = 进入。\nchannel 取值：0..39（0=2402MHz，步进 2MHz，39=2480MHz）。\npower 取值：0 = 0dBm / 1 = 3dBm / 2 = 4dBm / 3 = 6dBm。\n边界：mode∉{0,1} / channel>39 / power>3 → PARAM。',
          fields: [
          { key: 'mode', label: '定频', type: 'select', def: '1', options: [['0', '0 = 退出'], ['1', '1 = 进入']] },
          { key: 'channel', label: '信道 (0..39)', type: 'number', def: '19', min: 0, max: 39 },
          { key: 'power', label: '功率', type: 'select', def: '0',
            options: [['0', '0 = 0dBm'], ['1', '1 = 3dBm'], ['2', '2 = 4dBm'], ['3', '3 = 6dBm']] }
        ], run: function (c, v) { return c.setTestMode(parseInt(v.mode, 10), parseInt(v.channel, 10), parseInt(v.power, 10)); } }
      ]
    },
    {
      name: '透传', items: [
        { id: 'tunnel_usb', label: 'TUNNEL_USB', cmd: F.CMD.TUNNEL_USB,
          desc: '向 USB 侧透传原始报文；快速路径无 ACK。\n请求：0..64B 原始数据（超长截断）。未连接或发送失败静默丢弃。',
          fields: [
          { key: 'data', label: '数据(hex, ≤64B)', type: 'hex', def: 'A1B2C3D4', maxBytes: 64 }
        ], run: function (c, v) { return c.tunnelUsb(F.hexToBytes(v.data)).then(function () { return { text: '已发送 TUNNEL_USB' }; }); } },
        { id: 'tunnel_ble', label: 'TUNNEL_BLE', cmd: F.CMD.TUNNEL_BLE,
          desc: '向 BLE 侧透传原始报文；快速路径无 ACK。\n请求：0..32B 原始数据（超长截断）。未连接或发送失败静默丢弃。',
          fields: [
          { key: 'data', label: '数据(hex, ≤32B)', type: 'hex', def: 'A1B2C3D4', maxBytes: 32 }
        ], run: function (c, v) { return c.tunnelBle(F.hexToBytes(v.data)).then(function () { return { text: '已发送 TUNNEL_BLE' }; }); } }
      ]
    },
    {
      name: '危险操作（二次确认）', items: [
        { id: 'reset', label: 'RESET', cmd: F.CMD.RESET, dangerous: true,
          desc: 'MCU 软复位（port_reset）。\n请求：无 payload。\n应答：无——设备随即重启，链路短暂中断。',
          confirm: 'RESET 会立即复位模组，链路将短暂中断。确认发送？', fields: [],
          run: function (c) { return c.reset(); } },
        { id: 'enter_boot', label: 'ENTER_BOOT', cmd: F.CMD.ENTER_BOOT, dangerous: true,
          desc: '跳转 bootloader（量产烧录用）。\n请求：无 payload。\n现象：先回 ACK，4 轮 poll+flush+delay(6ms) 约 60ms 后释放 HID 并 port_enter_bootloader()。\n应答：ACK，随后进入 bootloader。',
          confirm: 'ENTER_BOOT 会拉低 PB22 并复位进入 ROM ISP。确认发送？', fields: [],
          run: function (c) { return c.enterBoot(); } },
        { id: 'unpair_all', label: 'UNPAIR 全部', cmd: F.CMD.UNPAIR, dangerous: true,
          desc: '清除全部绑定（等价 UNPAIR 0xFF）。\n请求：无 payload（内部下发 0xFF）。\n现象：先 ACK 后 EVT_STATE，需重新配对。',
          confirm: '将清除全部绑定信息，需重新配对。确认执行？', fields: [],
          run: function (c) { return c.unpair(0xFF); } }
      ]
    }
  ];

  /* 供 HID 直发引用的快速命令（命令台内提示跳转 HID Tab） */
  COMMANDS.hidRelease = { label: 'HID_RELEASE', cmd: F.CMD.HID_RELEASE };

  var api = { Commands: Commands, COMMANDS: COMMANDS, asciiBytes: asciiBytes };
  global.DbgCommands = api;
  if (typeof module === 'object' && module.exports) { module.exports = api; }
})(typeof window !== 'undefined' ? window : globalThis);
