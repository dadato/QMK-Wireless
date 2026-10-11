/* 调试工具 Web 应用 —— UI 编排（Tab / 连接区 / 状态仪表盘 / 日志 / 事件绑定）
 *
 * 依赖（按 index.html 顺序加载）：DbgCrc → DbgFrame → DbgDecode → DbgLink →
 * DbgCommands → DbgHid → DbgScenarios → DbgMonitor。
 * 规格见同目录 SPEC.md。
 */
(function () {
  'use strict';

  var F = window.DbgFrame;
  var D = window.DbgDecode;
  var Link = window.DbgLink.Link;
  var Commands = window.DbgCommands.Commands;
  var HID = window.DbgHid;
  var SCN = window.DbgScenarios;
  var Via = window.DbgVia;
  var Sim = window.DbgViaSim;

  function $(id) { return document.getElementById(id); }
  function hex16(n) { return '0x' + (n & 0xFFFF).toString(16).toUpperCase().padStart(4, '0'); }
  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  /* ============================ 日志 ============================ */
  var logBox = $('logBox');
  var logEntries = [];      /* {level, dir, text, time} */
  var logFilter = 'ALL';

  function ts() {
    var d = new Date();
    function p(n, w) { return String(n).padStart(w, '0'); }
    return p(d.getHours(), 2) + ':' + p(d.getMinutes(), 2) + ':' + p(d.getSeconds(), 2) +
      '.' + p(d.getMilliseconds(), 3);
  }

  /** @param {'INFO'|'OK'|'WARN'|'ERR'} level @param {string} msg @param {string} [dir] */
  function log(level, msg, dir) {
    var t = ts();
    logEntries.push({ level: level, dir: dir || '', text: msg, time: t });
    if (logBox) {
      var line = document.createElement('div');
      line.dataset.level = level;
      var eT = document.createElement('span'); eT.className = 't'; eT.textContent = t;
      var eD = document.createElement('span'); eD.className = 'dir'; eD.textContent = dir || '';
      var eL = document.createElement('span'); eL.className = 'lvl ' + level; eL.textContent = level;
      var eM = document.createElement('span'); eM.className = 'm'; eM.textContent = msg;
      line.appendChild(eT); line.appendChild(eD); line.appendChild(eL); line.appendChild(eM);
      if (logFilter !== 'ALL' && logFilter !== level) { line.classList.add('hidden'); }
      logBox.appendChild(line);
      logBox.scrollTop = logBox.scrollHeight;
      while (logBox.childElementCount > 3000) { logBox.removeChild(logBox.firstChild); }
    }
  }

  function applyLogFilter() {
    if (!logBox) { return; }
    var children = logBox.children;
    for (var i = 0; i < children.length; i++) {
      var lv = children[i].dataset.level;
      var ok = (logFilter === 'ALL' || lv === logFilter);
      children[i].classList.toggle('hidden', !ok);
    }
  }

  /* ============================ 兼容性检测 ============================ */
  var hasSerial = Link.isSupported();
  (function initCompat() {
    var box = $('compat');
    function badge(text, cls) {
      var b = document.createElement('span');
      b.className = 'badge ' + (cls || '');
      b.textContent = text;
      box.appendChild(b);
    }
    badge('WebSerial ' + (hasSerial ? '可用' : '不可用'), hasSerial ? 'ok' : 'err');
    badge(window.isSecureContext ? '安全上下文' : '非安全上下文', window.isSecureContext ? 'ok' : 'warn');
    if (!hasSerial) {
      $('portBtn').disabled = true;
      log('ERR', '当前浏览器不支持 WebSerial（请使用 Chrome/Edge），连接功能已禁用');
    }
    if (!window.isSecureContext) {
      log('WARN', '当前非安全上下文，WebSerial 可能被拒绝；请用 localhost 或 file:// 打开');
    }
  })();

  /* ============================ Tab 切换 ============================ */
  var tabs = document.querySelectorAll('.tab');
  Array.prototype.forEach.call(tabs, function (tab) {
    tab.addEventListener('click', function () {
      Array.prototype.forEach.call(tabs, function (t) { t.classList.remove('active'); });
      tab.classList.add('active');
      Array.prototype.forEach.call(document.querySelectorAll('.panel'), function (p) { p.classList.add('hidden'); });
      $(tab.dataset.panel).classList.remove('hidden');
    });
  });

  var subtabs = document.querySelectorAll('.subtab');
  Array.prototype.forEach.call(subtabs, function (st) {
    st.addEventListener('click', function () {
      Array.prototype.forEach.call(subtabs, function (t) { t.classList.remove('active'); });
      st.classList.add('active');
      Array.prototype.forEach.call(document.querySelectorAll('#panelHid .hid-panel'), function (p) { p.classList.add('hidden'); });
      $(st.dataset.sub).classList.remove('hidden');
    });
  });

  /* ============================ 监视器 ============================ */
  var monitor = new window.DbgMonitor.Monitor({
    listEl: $('monList'), emptyEl: $('monEmpty'),
    onCount: function (n) { $('monCount').textContent = n + ' 帧'; }
  });
  $('monClear').addEventListener('click', function () { monitor.clear(); });
  $('monPause').addEventListener('click', function () {
    var p = monitor.togglePause();
    $('monPause').textContent = p ? '继续' : '暂停';
    $('monPause').classList.toggle('primary', p);
  });
  $('monType').addEventListener('change', function () {
    monitor.setFilter($('monType').value, $('monText').value);
  });
  $('monText').addEventListener('input', function () {
    monitor.setFilter($('monType').value, $('monText').value);
  });
  $('monExportTxt').addEventListener('click', function () { download('frames.txt', monitor.exportTxt(), 'text/plain'); });
  $('monExportJson').addEventListener('click', function () { download('frames.json', monitor.exportJson(), 'application/json'); });

  /* ============================ 状态仪表盘 ============================ */
  function setVal(elId, text) { $(elId).textContent = text; }
  function setStat(statId, cls) {
    var el = $(statId);
    el.classList.remove('good', 'bad', 'warn');
    if (cls) { el.classList.add(cls); }
  }
  function setVersion(v) {
    setVal('stVersion', v.text);
    $('verText').textContent = '版本 ' + v.text;
  }
  function setStatusView(s) {
    setVal('stMode', s.modeName);
    setStat('statMode', s.mode === F.MODE.SLEEP ? 'warn' : '');
    setVal('stConnected', s.connected ? '已连接' : '未连接');
    setStat('statConn', s.connected ? 'good' : '');
    setVal('stProfile', s.profile);
    setVal('stBattery', s.battery + '%');
    setStat('statBattery', s.battery < 20 ? 'bad' : (s.battery < 40 ? 'warn' : 'good'));
  }
  function setLeds(l) {
    setVal('stLeds', 'Num ' + (l.num ? '●' : '○') + ' / Caps ' + (l.caps ? '●' : '○') + ' / Scroll ' + (l.scroll ? '●' : '○'));
  }
  /** BLE 多主机槽绑定：GET_BLE_SLOTS 应答 [s1..s4][last_active] */
  function setBleSlots(s) {
    var parts = [];
    for (var i = 0; i < s.slots.length; i++) { parts.push('槽' + (i + 1) + (s.slots[i] ? '●' : '○')); }
    parts.push('活跃 ' + (s.lastActive ? ('槽' + s.lastActive) : '无'));
    setVal('stBleSlots', parts.join(' '));
    setStat('statBleSlots', s.bound > 0 ? 'good' : '');
  }
  /** 拉取槽绑定状态；Dongle 侧不支持（ERR(STATE)）时提示 */
  async function refreshBleSlots() {
    if (!commands) { return; }
    try {
      setBleSlots(await commands.getBleSlots());
    } catch (e) {
      setVal('stBleSlots', '不支持');
      setStat('statBleSlots', 'warn');
    }
  }
  function updateCounters() {
    var s = link ? link.stats : null;
    setVal('cTx', s ? s.txFrames : 0);
    setVal('cRx', s ? s.rxFrames : 0);
    setVal('cCrc', s ? s.crcErrors : 0);
    setVal('cTimeout', s ? s.timeouts : 0);
    setVal('cErr', s ? s.errorsRx : 0);
    setVal('cEvt', s ? s.events : 0);
    setVal('cPong', s ? s.pongRx : 0);
  }

  /** 刷新状态快照（版本/状态/MAC/ADC/BLE 槽绑定） */
  async function refreshDashboard(full) {
    if (!commands) { return; }
    try {
      if (full) { setVersion(await commands.getVersion()); }
      var st = await commands.getStatus();
      setStatusView(st);
      var mac = await commands.getMac();
      setVal('stMac', mac.mac);
      setStat('statMac', mac.mac === '00:00:00:00:00:00' ? 'warn' : 'good');
      var adc = await commands.getAdcMv();
      setVal('stAdc', adc.mv + ' mV');
      await refreshBleSlots();
      log('OK', '状态快照：mode=' + st.modeName + ' connected=' + st.connected +
        ' profile=' + st.profile + ' battery=' + st.battery + '% MAC=' + mac.mac + ' ADC=' + adc.mv + 'mV');
    } catch (e) {
      log('WARN', '状态刷新失败：' + e.message);
    }
  }
  $('refreshBtn').addEventListener('click', function () { refreshDashboard(true); });

  /* ============================ 连接区 ============================ */
  var link = null;
  var commands = null;
  var localHbDisabled = true;    /* 「停发本机心跳」开关状态（Web 侧 1s PING）：单向机制下默认停发 */
  var healthDot = $('healthDot');
  var healthText = $('linkHealth');

  function updateLocalHbHint() {
    var el = $('localHbHint');
    if (el) {
      el.textContent = localHbDisabled
        ? '本机（Web）侧心跳：默认停发（单向，回 PONG 即保活）'
        : '本机（Web）侧心跳：每 1000ms 发送 PING';
    }
  }

  /* 本机心跳开关：独立于桥侧 SET_HEARTBEAT（0x0D），只控制 Web 是否周期发 PING */
  $('localHbToggle').addEventListener('change', function (e) {
    localHbDisabled = !!e.target.checked;
    if (link) { link.setLocalHeartbeat(!localHbDisabled); }
    updateLocalHbHint();
  });

  function setHealth(kind, text) {
    healthDot.className = 'dot ' + (kind || '');
    healthText.textContent = text;
  }

  /** 读取连接区所选波特率（默认 921600） */
  function selectedBaud() {
    var el = $('baudSel');
    var v = el ? parseInt(el.value, 10) : 921600;
    return (v > 0) ? v : 921600;
  }

  function setBaudLocked(locked) {
    var el = $('baudSel');
    if (el) { el.disabled = !!locked; }
  }

  $('portBtn').addEventListener('click', async function () {
    if (link && link.isConnected()) {
      try { await link.close(); } catch (e) { /* ignore */ }
      link = null; commands = null;
      $('portInfo').className = 'dev muted';
      $('portInfo').textContent = '未连接';
      $('portBtn').textContent = '选择串口';
      $('portBtn').classList.add('primary');
      setBaudLocked(false);
      setHealth('', '链路：未连接');
      setVal('verText', '版本 --');
      updateCounters();
      log('INFO', '串口已断开');
      return;
    }
    try {
      var lk = new Link({
        baud: selectedBaud(),
        log: function (level, msg) { log(level, msg, '←'); },
        onFrame: function (rec) { monitor.addFrame(rec); },
        onCrcError: function (raw) { monitor.addCrc(raw); updateCounters(); },
        onEvent: function (fr) { onBridgeEvent(fr); },
        onDisconnect: function () {
          log('WARN', '链路断开（设备复位或串口拔出）');
          setHealth('err', '链路：已断开');
          $('portBtn').textContent = '选择串口';
          $('portBtn').classList.add('primary');
          setBaudLocked(false);
        }
      });
      var port = await lk.connect();
      link = lk;
      commands = new Commands(link);
      lk.setLocalHeartbeat(!localHbDisabled);   /* 始终同步开关状态：默认停发（单向） */
      updateLocalHbHint();
      var info = port.getInfo ? port.getInfo() : {};
      $('portInfo').className = 'dev';
      $('portInfo').textContent = 'COM 已打开（' + lk.baud + ' 8N1）' +
        (info.usbVendorId ? '  USB ' + hex16(info.usbVendorId) + ':' + hex16(info.usbProductId) : '');
      $('portBtn').textContent = '断开';
      $('portBtn').classList.remove('primary');
      setBaudLocked(true);
      log('OK', '串口已连接，开始状态快照');
      await refreshDashboard(true);
    } catch (e) {
      link = null; commands = null;
      $('portInfo').className = 'dev muted';
      $('portInfo').textContent = '未连接';
      var cancelled = e && e.name === 'NotFoundError';
      log(cancelled ? 'WARN' : 'ERR', cancelled ? '未选择串口' : ('串口打开失败：' + e.message));
    }
  });

  /** 桥事件 → 仪表盘 + 日志 */
  function onBridgeEvent(fr) {
    try {
      /* LD7 VIA 模拟器（通路 C）：隧道事件 = 桌面 VIA 经无线/模组转发来的 VIA Raw HID，
       * 由模拟器按 LD7 的 VIA 数据应答，并经 CMD_TUNNEL_* 回发模组。 */
      if (viaSim && viaSim.running &&
          (fr.cmd === F.EVT.TUNNEL_USB || fr.cmd === F.EVT.TUNNEL_BLE)) {
        onViaSimTunnel(fr);
        return;
      }
      if (fr.cmd === F.EVT.STATE) {
        var s = D.decodeState(fr.payload);
        setStatusView(s);
        refreshBleSlots();
        log('INFO', 'EVT_STATE mode=' + s.modeName + ' connected=' + s.connected +
          ' profile=' + s.profile + ' battery=' + s.battery + '%', '←');
      } else if (fr.cmd === F.EVT.BATTERY) {
        var b = D.decodeBattery(fr.payload);
        setVal('stBattery', b.percent + '%');
        setVal('stAdc', b.mv + ' mV');
        log('INFO', 'EVT_BATTERY ' + b.mv + 'mV / ' + b.percent + '%', '←');
      } else if (fr.cmd === F.EVT.LEDS) {
        setLeds(D.decodeLeds(fr.payload));
        log('INFO', 'EVT_LEDS ' + F.bytesToHex(fr.payload), '←');
      } else if (fr.cmd === F.EVT.PAIR_RESULT) {
        var r = D.decodePairResult(fr.payload);
        log(r.ok ? 'OK' : 'WARN', 'EVT_PAIR_RESULT ' + (r.ok ? '成功' : ('失败(' + r.result + ')')) + ' profile=' + r.profile, '←');
      } else if (fr.cmd === F.EVT.ERROR) {
        var er = D.decodeError(fr.payload);
        log('ERR', 'EVT_ERROR ' + er.name, '←');
      } else if (fr.cmd === F.EVT.HOST_POWER) {
        var hp = D.decodeHostPower(fr.payload);
        log('INFO', 'EVT_HOST_POWER ' + hp.name + '（' + (hp.awake ? '运行/唤醒' : '挂起') + '）', '←');
      }
    } catch (e) { /* ignore */ }
  }

  /* 健康 & 计数轮询 */
  setInterval(function () {
    if (!link || !link.isConnected()) {
      if (!link) { setHealth('', '链路：未连接'); }
      return;
    }
    if (link.healthOk()) { setHealth('ok', '链路：正常'); }
    else if (link.stats.pingRx > 0) { setHealth('warn', '链路：心跳超时'); }
    else { setHealth('warn', '链路：等待桥 PING'); }
    updateCounters();
  }, 1000);

  /* ============================ 命令台 ============================ */
  function buildField(f) {
    var wrap = document.createElement('label');
    wrap.className = 'f';
    wrap.appendChild(document.createTextNode(f.label));
    var el;
    if (f.type === 'select') {
      el = document.createElement('select');
      f.options.forEach(function (op) {
        var o = document.createElement('option');
        o.value = op[0]; o.textContent = op[1];
        el.appendChild(o);
      });
      el.value = f.def;
      wrap.appendChild(el);
    } else if (f.type === 'checkbox') {
      wrap.className = 'chk';
      wrap.textContent = '';
      el = document.createElement('input');
      el.type = 'checkbox';
      el.checked = !!f.def;
      wrap.appendChild(el);
      wrap.appendChild(document.createTextNode(f.label));
    } else {
      el = document.createElement('input');
      el.type = (f.type === 'number') ? 'number' : 'text';
      if (f.type === 'hex') { el.className = 'hex'; }
      if (f.min !== undefined) { el.min = f.min; }
      if (f.max !== undefined) { el.max = f.max; }
      if (f.maxlength !== undefined) { el.maxLength = f.maxlength; }
      el.value = f.def;
      wrap.appendChild(el);
    }
    return { field: f, el: el, wrap: wrap };
  }

  function readFields(built) {
    var vals = {};
    built.forEach(function (b) {
      var f = b.field;
      if (f.type === 'checkbox') { vals[f.key] = b.el.checked; return; }
      var raw = String(b.el.value || '').trim();
      if (f.type === 'number') { vals[f.key] = parseInt(raw || '0', 10); return; }
      if (f.type === 'hex') {
        var bytes;
        try { bytes = F.hexToBytes(raw); }
        catch (e) { throw new Error(f.label + ' 非法 hex'); }
        if (f.maxBytes && bytes.length > f.maxBytes) {
          throw new Error(f.label + ' 超过 ' + f.maxBytes + 'B');
        }
        vals[f.key] = raw;
        vals[f.key + '__bytes'] = bytes;
        return;
      }
      vals[f.key] = raw;
    });
    return vals;
  }

  function renderCommands() {
    var root = $('cmdGroups');
    window.DbgCommands.COMMANDS.forEach(function (group) {
      var g = document.createElement('div');
      g.className = 'cmd-group';
      var h = document.createElement('h3');
      h.textContent = group.name;
      g.appendChild(h);
      var items = document.createElement('div');
      items.className = 'cmd-items';
      group.items.forEach(function (item) { items.appendChild(buildCmdItem(item)); });
      g.appendChild(items);
      root.appendChild(g);
    });
  }

  function buildCmdItem(item) {
    var box = document.createElement('div');
    box.className = 'cmd-item' + (item.dangerous ? ' danger' : '');
    var head = document.createElement('div');
    head.className = 'cmd-head';
    var lbl = document.createElement('span'); lbl.className = 'lbl'; lbl.textContent = item.label;
    var code = document.createElement('span'); code.className = 'code'; code.textContent = '0x' + item.cmd.toString(16).toUpperCase();
    head.appendChild(lbl); head.appendChild(code);
    box.appendChild(head);
    var desc = document.createElement('div'); desc.className = 'cmd-desc'; desc.textContent = item.desc;
    box.appendChild(desc);

    var built = [];
    if (item.fields.length) {
      var fwrap = document.createElement('div');
      fwrap.className = 'cmd-fields';
      item.fields.forEach(function (f) { var b = buildField(f); built.push(b); fwrap.appendChild(b.wrap); });
      box.appendChild(fwrap);
    }

    var row = document.createElement('div');
    row.className = 'row';
    var btn = document.createElement('button');
    btn.className = 'btn ' + (item.dangerous ? 'danger' : 'small');
    btn.type = 'button';
    btn.textContent = item.dangerous ? '执行（危险）' : '执行';
    row.appendChild(btn);
    box.appendChild(row);

    var res = document.createElement('div'); res.className = 'cmd-result';
    box.appendChild(res);

    btn.addEventListener('click', function () { runCommandItem(item, built, res, btn); });
    return box;
  }

  async function runCommandItem(item, built, resEl, btn) {
    if (!commands) { setResult(resEl, '未连接串口', 'err'); return; }
    if (item.dangerous && !window.confirm(item.confirm || '确认执行该操作？')) {
      setResult(resEl, '已取消', ''); return;
    }
    var vals;
    try { vals = readFields(built); }
    catch (e) { setResult(resEl, e.message, 'err'); return; }

    btn.disabled = true;
    setResult(resEl, '执行中…', '');
    try {
      var r = await item.run(commands, vals);
      var reqHex = link.lastTxRecord ? F.bytesToHex(link.lastTxRecord.raw) : '(无)';
      var lines = ['请求: ' + reqHex];
      if (r && r.frame) {
        lines.push('应答: ' + F.bytesToHex(F.encode(r.frame.cmd, r.frame.seq, r.frame.flags, r.frame.payload)));
        lines.push('解码: ' + D.summarize(r.frame));
      }
      lines.push('结果: ' + ((r && r.text) || 'OK'));
      setResult(resEl, lines.join('\n'), 'ok');
      log('OK', '命令 ' + item.label + ' → ' + ((r && r.text) || 'OK'), '→');
    } catch (e) {
      setResult(resEl, '错误: ' + (e.code !== null && e.code !== undefined ? (F.errCodeName(e.code) + ' — ') : '') + e.message, 'err');
      log('ERR', '命令 ' + item.label + ' 失败：' + e.message, '→');
    } finally {
      btn.disabled = false;
    }
  }

  function setResult(el, text, cls) {
    el.className = 'cmd-result' + (cls ? ' ' + cls : '');
    el.textContent = text;
  }

  /* 原始帧编辑器 */
  $('rawSend').addEventListener('click', async function () {
    if (!link || !link.isConnected()) { setResult($('rawResult'), '未连接串口', 'err'); return; }
    var cmd, seq, flags, payload;
    try {
      var cb = F.hexToBytes($('rawCmd').value); if (!cb.length) { throw new Error('CMD 不能为空'); }
      cmd = cb[0];
      var sb = F.hexToBytes($('rawSeq').value); seq = sb.length ? sb[0] : link.nextSeq();
      var fb = F.hexToBytes($('rawFlags').value); flags = fb.length ? fb[0] : 0;
      payload = F.hexToBytes($('rawPayload').value);
    } catch (e) {
      setResult($('rawResult'), '参数错误：' + e.message, 'err'); return;
    }
    setResult($('rawResult'), '发送中…', '');
    try {
      var fr;
      if ($('rawWaitAck').checked) {
        fr = await link.requestRaw(cmd, seq, flags, payload, 600, 1);
        setResult($('rawResult'), 'TX: ' + F.bytesToHex(F.encode(cmd, seq, flags, payload)) +
          '\nRX: ' + F.bytesToHex(F.encode(fr.cmd, fr.seq, fr.flags, fr.payload)) +
          '\n解码: ' + D.summarize(fr), 'ok');
      } else {
        await link.sendRaw(cmd, seq, flags, payload);
        setResult($('rawResult'), 'TX: ' + F.bytesToHex(F.encode(cmd, seq, flags, payload)) + '（无应答）', 'ok');
      }
    } catch (e) {
      setResult($('rawResult'), '错误: ' + e.message, 'err');
    }
  });
  $('rawRelease').addEventListener('click', async function () {
    if (!commands) { setResult($('rawResult'), '未连接串口', 'err'); return; }
    await commands.hidRelease();
    setResult($('rawResult'), '已发送 HID_RELEASE', 'ok');
  });

  /* ============================ HID 模拟 ============================ */
  var MOD_NAMES = ['LCtrl', 'LShift', 'LAlt', 'LGui', 'RCtrl', 'RShift', 'RAlt', 'RGui'];

  function renderMods() {
    var box = $('hidMods');
    MOD_NAMES.forEach(function (n, i) {
      var lab = document.createElement('label');
      lab.className = 'chk';
      var cb = document.createElement('input');
      cb.type = 'checkbox'; cb.dataset.bit = i;
      lab.appendChild(cb); lab.appendChild(document.createTextNode(n));
      box.appendChild(lab);
    });
  }
  function renderKeys() {
    var box = $('hidKeys');
    for (var i = 0; i < 6; i++) {
      var lab = document.createElement('label');
      lab.className = 'f';
      lab.appendChild(document.createTextNode('键' + (i + 1) + '(hex)'));
      var inp = document.createElement('input');
      inp.type = 'text'; inp.className = 'hex'; inp.value = '00'; inp.dataset.key = i;
      inp.style.width = '70px';
      lab.appendChild(inp);
      box.appendChild(lab);
    }
  }
  function renderNkroGrid() {
    var box = $('hidNkroGrid');
    for (var i = 0; i < 128; i++) {
      var usage = i + 4;
      var lab = document.createElement('label');
      lab.className = 'keycell';
      var cb = document.createElement('input');
      cb.type = 'checkbox'; cb.dataset.nkro = i;
      lab.appendChild(cb);
      lab.appendChild(document.createTextNode('0x' + usage.toString(16).toUpperCase().padStart(2, '0')));
      box.appendChild(lab);
    }
  }
  renderMods(); renderKeys(); renderNkroGrid();

  function readMods() {
    var m = 0;
    Array.prototype.forEach.call($('hidMods').querySelectorAll('input'), function (cb) {
      if (cb.checked) { m |= (1 << parseInt(cb.dataset.bit, 10)); }
    });
    return m;
  }
  function readKeys() {
    var keys = [];
    Array.prototype.forEach.call($('hidKeys').querySelectorAll('input'), function (inp) {
      var v = parseInt(inp.value, 16) || 0;
      if (v) { keys.push(v); }
    });
    return keys;
  }

  $('hidKbSend').addEventListener('click', function () {
    if (!requireLink($('hidReportResult'))) { return; }
    var mod = readMods(), keys = readKeys();
    commands.hidKeyboard(keys, mod);
    setProg($('hidReportResult'), 'HID_KEYBOARD mod=0x' + mod.toString(16) + ' keys=' + F.bytesToHex(Uint8Array.from(keys)));
  });
  $('hidKbClear').addEventListener('click', function () {
    Array.prototype.forEach.call($('hidMods').querySelectorAll('input'), function (cb) { cb.checked = false; });
    Array.prototype.forEach.call($('hidKeys').querySelectorAll('input'), function (inp) { inp.value = '00'; });
  });
  $('hidNkroSend').addEventListener('click', function () {
    if (!requireLink($('hidReportResult'))) { return; }
    var bitmap = new Uint8Array(16);
    var n = 0;
    Array.prototype.forEach.call($('hidNkroGrid').querySelectorAll('input'), function (cb) {
      if (cb.checked) { var idx = parseInt(cb.dataset.nkro, 10); bitmap[idx >> 3] |= (1 << (idx & 7)); n++; }
    });
    commands.hidNkro(bitmap);
    setProg($('hidReportResult'), 'HID_NKRO ' + n + ' 键：' + F.bytesToHex(bitmap));
  });
  $('hidNkroClear').addEventListener('click', function () {
    Array.prototype.forEach.call($('hidNkroGrid').querySelectorAll('input'), function (cb) { cb.checked = false; });
  });
  $('hidConsumerSend').addEventListener('click', function () {
    if (!requireLink($('hidReportResult'))) { return; }
    commands.hidConsumer(F.hexToBytes($('hidConsumer').value));
    setProg($('hidReportResult'), 'HID_CONSUMER ' + $('hidConsumer').value);
  });
  $('hidSystemSend').addEventListener('click', function () {
    if (!requireLink($('hidReportResult'))) { return; }
    commands.hidSystem(parseInt($('hidSystem').value, 16) || 0);
    setProg($('hidReportResult'), 'HID_SYSTEM 0x' + (parseInt($('hidSystem').value, 16) || 0).toString(16));
  });
  $('hidMouseSend').addEventListener('click', function () {
    if (!requireLink($('hidReportResult'))) { return; }
    var btn = 0;
    if ($('hidMouseBtn0').checked) { btn |= 0x01; }
    if ($('hidMouseBtn1').checked) { btn |= 0x02; }
    if ($('hidMouseBtn2').checked) { btn |= 0x04; }
    var r = Uint8Array.of(btn, parseInt($('hidDx').value, 10) & 0xFF,
      parseInt($('hidDy').value, 10) & 0xFF, parseInt($('hidWheel').value, 10) & 0xFF);
    commands.hidMouse(r);
    setProg($('hidReportResult'), 'HID_MOUSE ' + F.bytesToHex(r));
  });
  $('hidReleaseBtn').addEventListener('click', function () {
    if (!requireLink($('hidReportResult'))) { return; }
    commands.hidRelease();
    setProg($('hidReportResult'), 'HID_RELEASE 已发送（清除去重缓存）');
  });

  /* 文本打字 */
  var typeCancelled = false;
  $('hidTypeSend').addEventListener('click', async function () {
    if (!requireLink($('hidTypeResult'))) { return; }
    var text = $('hidText').value;
    var rate = parseInt($('hidRate').value, 10) || 12;
    var hold = parseInt($('hidHold').value, 10) || 15;
    var delayMs = parseInt($('hidTypeDelay').value, 10);
    if (isNaN(delayMs) || delayMs < 0) { delayMs = 0; }
    if ($('hidWaitReady').checked) {
      var ok = await waitReady(5000);
      if (!ok) { setProg($('hidTypeResult'), '链路未就绪（未收到桥 PING），仍将发送…'); }
    }
    typeCancelled = false;
    $('hidTypeSend').disabled = true; $('hidTypeStop').disabled = false;
    var sent = 0, total = text.length;
    try {
      var remain = delayMs;
      while (remain > 0 && !typeCancelled) {
        setProg($('hidTypeResult'), '触发延迟中… ' + (remain / 1000).toFixed(1) + 's 后开始打字');
        var step = Math.min(100, remain);
        await sleep(step);
        remain -= step;
      }
      if (typeCancelled) {
        setProg($('hidTypeResult'), '已取消（触发延迟中停止）');
      } else {
        var r = await HID.typeText(commands, text, {
          rateHz: rate, holdMs: hold,
          isCancelled: function () { return typeCancelled; },
          onChar: function (ch, ok) {
            if (ok) { sent++; if (sent % 5 === 0 || sent === total) { setProg($('hidTypeResult'), '打字中 ' + sent + '/' + total); } }
          }
        });
        setProg($('hidTypeResult'), '已发送 ' + r.sent + '/' + r.chars + ' 字符' +
          (r.skipped.length ? ('；跳过不支持字符：' + r.skipped.join(' ')) : '') +
          (typeCancelled ? '（已停止）' : ''));
        log('OK', 'HID 打字：' + r.sent + ' 字符' + (r.skipped.length ? ('，跳过 ' + r.skipped.join('')) : ''), '→');
      }
    } finally {
      $('hidTypeSend').disabled = false; $('hidTypeStop').disabled = true;
    }
  });
  $('hidTypeStop').addEventListener('click', function () { typeCancelled = true; });

  /* 宏脚本 */
  var macroCancelled = false;
  $('hidMacroRun').addEventListener('click', async function () {
    if (!requireLink($('hidMacroResult'))) { return; }
    macroCancelled = false;
    $('hidMacroRun').disabled = true; $('hidMacroStop').disabled = false;
    try {
      var r = await HID.runMacro(commands, $('hidMacro').value, {
        loop: parseInt($('hidMacroLoop').value, 10) || 1,
        isCancelled: function () { return macroCancelled; }
      });
      setProg($('hidMacroResult'), '已执行 ' + r.steps + ' 步' + (macroCancelled ? '（已停止）' : '') +
        (r.errors.length ? ('\n问题：\n' + r.errors.join('\n')) : ''));
      log(r.errors.length ? 'WARN' : 'OK', '宏脚本执行 ' + r.steps + ' 步' + (r.errors.length ? ('，' + r.errors.length + ' 处问题') : ''), '→');
    } finally {
      $('hidMacroRun').disabled = false; $('hidMacroStop').disabled = true;
    }
  });
  $('hidMacroStop').addEventListener('click', function () { macroCancelled = true; });

  /* 压力测试 */
  var stressCancelled = false;
  $('hidStressRun').addEventListener('click', async function () {
    if (!requireLink($('hidStressResult'))) { return; }
    var usages = $('hidStressUsages').value.split(',').map(function (s) {
      s = s.trim(); return s ? parseInt(s, 16) : 0;
    }).filter(function (u) { return u > 0; });
    if (!usages.length) { usages = [0x04]; }
    stressCancelled = false;
    $('hidStressRun').disabled = true; $('hidStressStop').disabled = false;
    try {
      var r = await HID.stress(commands, {
        usages: usages,
        rateHz: parseInt($('hidStressRate').value, 10) || 10,
        mode: $('hidStressMode').value,
        seconds: parseInt($('hidStressSeconds').value, 10) || 5,
        count: parseInt($('hidStressCount').value, 10) || 100,
        holdMs: parseInt($('hidStressHold').value, 10) || 10,
        isCancelled: function () { return stressCancelled; },
        onTick: function (k) { setProg($('hidStressResult'), '已发送 ' + k + ' 键…'); }
      });
      setProg($('hidStressResult'), '完成 ' + r.keys + ' 键 / ' + (r.elapsedMs / 1000).toFixed(2) + 's（' +
        r.keysPerSec.toFixed(1) + ' keys/s）· 发送帧 ' + r.frames + ' · ERR ' + r.errs +
        (r.cancelled ? '（已停止）' : ''));
      log('OK', '压力测试 ' + r.keys + ' 键，' + r.keysPerSec.toFixed(1) + ' keys/s，ERR=' + r.errs, '→');
    } finally {
      $('hidStressRun').disabled = false; $('hidStressStop').disabled = true;
    }
  });
  $('hidStressStop').addEventListener('click', function () { stressCancelled = true; });

  function setProg(el, text) { el.textContent = text; }
  function requireLink(el) {
    if (!commands) { if (el) { setProg(el, '未连接串口'); } log('WARN', '未连接串口，操作被忽略'); return false; }
    return true;
  }
  async function waitReady(timeoutMs) {
    var t0 = Date.now();
    while (Date.now() - t0 < timeoutMs) {
      if (link && link.healthOk()) { return true; }
      await sleep(150);
    }
    return false;
  }

  /* ============================ 场景 ============================ */
  function renderScenarios() {
    var list = $('scnList');
    SCN.LIST.forEach(function (spec) {
      var item = document.createElement('div');
      item.className = 'scn-item' + (spec.destructive ? ' destructive' : '');
      var head = document.createElement('div');
      head.className = 'scn-head';
      var left = document.createElement('div');
      var title = document.createElement('div'); title.className = 'scn-title'; title.textContent = spec.label;
      var hint = document.createElement('div'); hint.className = 'scn-hint'; hint.textContent = spec.hint;
      left.appendChild(title); left.appendChild(hint);
      var right = document.createElement('div');
      right.className = 'row';
      var badge = document.createElement('span'); badge.className = 'scn-badge'; badge.textContent = '未运行';
      var btn = document.createElement('button');
      btn.className = 'btn ' + (spec.destructive ? 'danger' : 'small');
      btn.type = 'button'; btn.textContent = spec.destructive ? '运行（破坏性）' : '运行';
      right.appendChild(badge); right.appendChild(btn);
      head.appendChild(left); head.appendChild(right);
      var results = document.createElement('div'); results.className = 'scn-results';
      item.appendChild(head); item.appendChild(results);
      btn.addEventListener('click', function () { runOneScenario(spec, badge, results, btn); });
      list.appendChild(item);
    });
  }

  async function runOneScenario(spec, badge, resultsEl, btn) {
    if (!commands) { log('WARN', '未连接串口，无法运行场景'); return; }
    if (spec.destructive && !window.confirm('该场景具破坏性（' + spec.label + '）。确认运行？')) { return; }
    badge.className = 'scn-badge run'; badge.textContent = '运行中';
    resultsEl.innerHTML = '';
    btn.disabled = true;
    try {
      var r = await SCN.runScenario(spec.id, commands, link, {
        onResult: function (res) { appendScenarioResult(resultsEl, res); }
      });
      badge.className = 'scn-badge ' + (r.failed ? 'fail' : 'pass');
      badge.textContent = r.failed ? ('FAIL ' + r.failed) : ('PASS ' + r.passed);
      log(r.failed ? 'ERR' : 'OK', '场景 ' + spec.id + '：' + (r.failed ? (r.failed + ' 项失败') : ('全部通过（' + r.passed + ' 项）')));
    } catch (e) {
      badge.className = 'scn-badge fail'; badge.textContent = 'FAIL';
      appendScenarioResult(resultsEl, { ok: false, name: '场景异常', detail: e.message });
    } finally {
      btn.disabled = false;
    }
  }

  function appendScenarioResult(container, res) {
    var row = document.createElement('div');
    row.className = 'scn-res ' + (res.ok === null ? 'info' : (res.ok ? 'pass' : 'fail'));
    var tag = document.createElement('span'); tag.className = 'tag';
    tag.textContent = res.ok === null ? 'INFO' : (res.ok ? 'PASS' : 'FAIL');
    var txt = document.createElement('span'); txt.className = 'txt'; txt.textContent = res.name;
    var det = document.createElement('span'); det.className = 'det'; det.textContent = res.detail ? ('　' + res.detail) : '';
    row.appendChild(tag); row.appendChild(txt); row.appendChild(det);
    container.appendChild(row);
  }

  $('scnRunAll').addEventListener('click', async function () {
    if (!commands) { log('WARN', '未连接串口，无法运行场景'); return; }
    $('scnRunAll').disabled = true;
    var totalPass = 0, totalFail = 0;
    setProg($('scnSummary'), '全部运行中…');
    try {
      var out = await SCN.runAll(commands, link, {
        onResult: function () {},
        onScenario: function (id, phase, r) {
          if (phase === 'done') {
            totalPass += r.passed; totalFail += r.failed;
            log(r.failed ? 'ERR' : 'OK', '场景 ' + id + '：' + (r.failed ? (r.failed + ' 项失败') : ('通过 ' + r.passed + ' 项')));
          }
        }
      });
      var _ = out;
      setProg($('scnSummary'), '全部运行完成：PASS ' + totalPass + ' · FAIL ' + totalFail);
      log(totalFail ? 'WARN' : 'OK', '非破坏性场景集合完成：PASS=' + totalPass + ' FAIL=' + totalFail);
      /* 同步各卡片徽标 */
      Array.prototype.forEach.call(document.querySelectorAll('#scnList .scn-item'), function (item, i) {
        var spec = SCN.LIST[i];
        var rec = out.filter(function (x) { return x.id === spec.id; })[0];
        var badge = item.querySelector('.scn-badge');
        if (rec && badge) { badge.className = 'scn-badge ' + (rec.failed ? 'fail' : 'pass'); badge.textContent = rec.failed ? ('FAIL ' + rec.failed) : ('PASS ' + rec.passed); }
      });
    } finally {
      $('scnRunAll').disabled = false;
    }
  });

  /* ============================ VIA 隧道回路测试 ============================ */
  var viaHid = null;
  var viaBle = null;
  var viaSim = null;   /* LD7 键盘 VIA 数据模拟器（通路 C，经串口扮演 SK32/LD7） */

  function viaOpts() {
    var timeoutMs = parseInt($('viaTimeout').value, 10) || 3000;
    var retries = parseInt($('viaRetries').value, 10);
    if (isNaN(retries)) { retries = 2; }
    return { timeoutMs: timeoutMs, retries: retries };
  }
  function viaLog(level, msg) { log(level, msg, 'VIA'); }
  function viaBadge(el, text, cls) {
    el.className = 'scn-badge' + (cls ? ' ' + cls : '');
    el.textContent = text;
  }

  /* 启动：写入自检报文预览，并按浏览器能力启用/禁用两条通路 */
  (function initVia() {
    $('viaTxHex').value = Via.hex(Via.buildSelfTest());

    if (!Via.ViaHid.isSupported()) {
      $('viaHidConn').disabled = true;
      $('viaHidDiag').disabled = true;
      viaBadge($('viaHidState'), '不支持', 'fail');
      setResult($('viaHidResult'), '当前浏览器不支持 WebHID（需 Chrome/Edge，且为安全上下文）', 'err');
      viaLog('WARN', 'WebHID 不可用，通路 A（Dongle）已禁用');
    }
    if (!Via.ViaBle.isSupported()) {
      $('viaBleConn').disabled = true;
      viaBadge($('viaBleState'), '不支持', 'fail');
      setResult($('viaBleResult'), '当前浏览器不支持 WebHID（需 Chrome/Edge，且为安全上下文）', 'err');
      viaLog('WARN', 'WebHID 不可用，通路 B（BLE 模组）已禁用');
    }
    updateSoakButtons();
  })();

  async function viaHidToggle() {
    var btn = $('viaHidConn');
    if (viaHid && viaHid.isConnected()) {
      await viaHid.close();
      viaHid = null;
      viaBadge($('viaHidState'), '未连接', '');
      setResult($('viaHidResult'), '已断开', '');
      btn.textContent = '连接 Dongle';
      $('viaHidRun').disabled = true;
      $('viaHidDiag').disabled = true;
      updateSoakButtons();
      viaLog('INFO', 'WebHID 已断开');
      return;
    }
    btn.disabled = true;
    var inst = new Via.ViaHid({
      log: viaLog,
      onDisconnect: function () {
        viaHid = null;
        viaBadge($('viaHidState'), '已断开', 'fail');
        setResult($('viaHidResult'), '设备已断开', 'err');
        $('viaHidConn').textContent = '连接 Dongle';
        $('viaHidRun').disabled = true;
        $('viaHidDiag').disabled = true;
        updateSoakButtons();
      }
    });
    try {
      var dev = await inst.requestDevice();
      await inst.open(dev);
      viaHid = inst;
      viaBadge($('viaHidState'), '已连接', 'pass');
      setResult($('viaHidResult'), '已连接：' + (dev.productName || 'Dongle'), 'ok');
      btn.textContent = '断开';
      $('viaHidRun').disabled = false;
      $('viaHidDiag').disabled = false;
      updateSoakButtons();
      viaLog('OK', 'WebHID 已连接：' + (dev.productName || '(未命名设备)'));
    } catch (e) {
      if (e && e.name === 'NotFoundError') { viaLog('WARN', '已取消选择 Dongle，未连接'); }
      else {
        viaBadge($('viaHidState'), '未连接', 'fail');
        setResult($('viaHidResult'), '连接失败：' + e.message, 'err');
        viaLog('ERR', 'WebHID 连接失败：' + e.message);
      }
    } finally {
      btn.disabled = false;
    }
  }

  async function viaBleToggle() {
    var btn = $('viaBleConn');
    if (viaBle && viaBle.isConnected()) {
      await viaBle.close();
      viaBle = null;
      viaBadge($('viaBleState'), '未连接', '');
      setResult($('viaBleResult'), '已断开', '');
      btn.textContent = '连接 BLE 模组';
      $('viaBleRun').disabled = true;
      updateSoakButtons();
      viaLog('INFO', 'WebHID（BLE 模组）已断开');
      return;
    }
    btn.disabled = true;
    var inst = new Via.ViaBle({
      log: viaLog,
      onDisconnect: function () {
        viaBle = null;
        viaBadge($('viaBleState'), '已断开', 'fail');
        setResult($('viaBleResult'), '设备已断开', 'err');
        $('viaBleConn').textContent = '连接 BLE 模组';
        $('viaBleRun').disabled = true;
        updateSoakButtons();
      }
    });
    try {
      var dev = await inst.requestDevice();
      await inst.open(dev);
      viaBle = inst;
      viaBadge($('viaBleState'), '已连接', 'pass');
      setResult($('viaBleResult'), '已连接：' + (dev.productName || dev.name || 'BLE 模组'), 'ok');
      btn.textContent = '断开';
      $('viaBleRun').disabled = false;
      updateSoakButtons();
      viaLog('OK', 'WebHID（BLE 模组）已连接：' + (dev.productName || dev.name || '(未命名设备)'));
    } catch (e) {
      if (e && e.name === 'NotFoundError') { viaLog('WARN', '已取消选择 BLE 模组，未连接'); }
      else {
        viaBadge($('viaBleState'), '未连接', 'fail');
        setResult($('viaBleResult'), '连接失败：' + e.message, 'err');
        viaLog('ERR', 'WebHID（BLE 模组）连接失败：' + e.message);
      }
    } finally {
      btn.disabled = false;
    }
  }

  async function viaRun(transport, resEl, btn) {
    if (!transport || !transport.isConnected()) { setResult(resEl, '设备未连接', 'err'); return; }
    btn.disabled = true;
    setResult(resEl, '发送自检报文…', '');
    try {
      var r = await Via.roundTrip(transport, viaOpts());
      var lines = [
        'TX: ' + Via.hex(r.tx),
        'RX: ' + Via.hex(r.rx),
        '判定: ' + (r.ok ? 'PASS' : 'FAIL') + ' — ' + r.detail,
        '耗时: ' + r.elapsedMs + 'ms'
      ];
      setResult(resEl, lines.join('\n'), r.ok ? 'ok' : 'err');
      viaLog(r.ok ? 'OK' : 'ERR', 'VIA 回路自检 ' + (r.ok ? 'PASS' : 'FAIL') + '：' + r.detail);
    } catch (e) {
      setResult(resEl, '失败：' + e.message, 'err');
      viaLog('ERR', 'VIA 回路自检失败：' + e.message);
    } finally {
      btn.disabled = false;
    }
  }

  /** 依据 Dongle 射频诊断快照给出通路 A 失败的判读（不经 2.4G，故超时也能读到） */
  function diagnoseHid(p) {
    var c = p.counters;
    if (!p.up) {
      return 'Dongle 射频未启动（up=0）：Dongle 未进入 2.4G 模式或初始化失败';
    }
    if (!p.bound) {
      return '未配对（bound=0）：键盘模组与 Dongle 未完成 2.4G 绑定，隧道数据帧被静默丢弃（rxDataBad=' + c.rxDataBad + '）';
    }
    if (c.rxAny === 0) {
      return '已配对但射频层未收到任何包（rxAny=0、txDoneFail=' + c.txDoneFail + '）：键盘模组可能未在 2.4G 模式、未上电或超距';
    }
    if (!p.conn) {
      return '已配对且能收包，但未建链（conn=0、hbMiss=' + p.hbMiss + '）：心跳丢失，链路不稳';
    }
    if (c.rxDataBad > 0) {
      return '链路在线，但存在 link_id 不符的丢包（rxDataBad=' + c.rxDataBad + '）';
    }
    return '链路在线（up/conn/bound 均正常）——回路失败疑点在 SK32 侧（模式/无线路径）';
  }

  async function viaHidDiagRun() {
    var btn = $('viaHidDiag');
    var resEl = $('viaHidDiagResult');
    if (!viaHid || !viaHid.isConnected()) { setResult(resEl, '设备未连接', 'err'); return; }
    btn.disabled = true;
    setResult(resEl, '读取 Dongle 本地诊断…', '');
    try {
      var o = viaOpts();
      var r = await viaHid.otaGetInfo(o.timeoutMs, o.retries);
      var p = r.parsed;
      var lines = ['TX: ' + Via.hex(Via.buildOtaGetInfo()), 'RX: ' + Via.hex(r.rx)];
      if (!p.ok) {
        lines.push('解析失败：' + p.detail);
        setResult(resEl, lines.join('\n'), 'err');
        viaLog('ERR', 'Dongle 诊断解析失败：' + p.detail);
        return;
      }
      var c = p.counters;
      lines.push('固件: mode=' + (p.mode === 0 ? 'App' : 'IAP') + ' fw_rev=' + p.fwRev +
        ' VID=0x' + p.vid.toString(16).toUpperCase().padStart(4, '0') +
        ' PID=0x' + p.pid.toString(16).toUpperCase().padStart(4, '0'));
      lines.push('射频: up=' + p.up + ' conn=' + p.conn + ' bound=' + p.bound +
        ' txBusy=' + p.txBusy + ' txQ=' + p.txQ + ' hbMiss=' + p.hbMiss + ' period=' + p.periodMs + 'ms');
      lines.push('计数: txSubmit ok/fail=' + c.txSubmitOk + '/' + c.txSubmitFail +
        ' txDone ok/fail=' + c.txDoneOk + '/' + c.txDoneFail + ' txTimeout=' + c.txTimeout);
      lines.push('       rxAny=' + c.rxAny + ' hopShut=' + c.hopShut + ' rxDataBad=' + c.rxDataBad);
      lines.push('判读: ' + diagnoseHid(p));
      setResult(resEl, lines.join('\n'), p.bound && p.up ? 'ok' : 'err');
      viaLog('OK', 'Dongle 2.4G 诊断已读取');
    } catch (e) {
      setResult(resEl, '失败：' + e.message, 'err');
      viaLog('ERR', 'Dongle 诊断读取失败：' + e.message);
    } finally {
      btn.disabled = false;
    }
  }

  $('viaHidConn').addEventListener('click', function () { viaHidToggle(); });
  $('viaHidRun').addEventListener('click', function () { viaRun(viaHid, $('viaHidResult'), $('viaHidRun')); });
  $('viaHidDiag').addEventListener('click', function () { viaHidDiagRun(); });
  $('viaBleConn').addEventListener('click', function () { viaBleToggle(); });
  $('viaBleRun').addEventListener('click', function () { viaRun(viaBle, $('viaBleResult'), $('viaBleRun')); });

  /* ---- 批量稳定性测试（通路 A/B，默认 10000 次） ---- */
  var viaSoakCancelled = false;
  var viaSoakRunning = false;

  /** 依据两条通路的连接态与运行态刷新按钮使能 */
  function updateSoakButtons() {
    var aOk = !!(viaHid && viaHid.isConnected());
    var bOk = !!(viaBle && viaBle.isConnected());
    $('viaSoakRunA').disabled = viaSoakRunning || !aOk;
    $('viaSoakRunB').disabled = viaSoakRunning || !bOk;
    $('viaSoakStop').disabled = !viaSoakRunning;
  }

  function fmtMs(v) { return (v === null || v === undefined) ? '—' : (Math.round(v * 100) / 100) + 'ms'; }
  function fmtPct(v) { return (v === null || v === undefined) ? '—' : (v * 100).toFixed(3) + '%'; }

  async function runSoak(which) {
    var transport = (which === 'A') ? viaHid : viaBle;
    var resEl = $('viaSoakResult');
    var barEl = $('viaSoakBar');
    var progEl = $('viaSoakProgress');
    var label = (which === 'A') ? '通路 A（2.4G）' : '通路 B（BLE）';
    if (!transport || !transport.isConnected()) { setResult(resEl, '设备未连接', 'err'); return; }

    var count = parseInt($('viaSoakCount').value, 10) || 10000;
    var gapMs = parseInt($('viaSoakGap').value, 10);
    if (isNaN(gapMs)) { gapMs = 1; }
    var timeoutMs = parseInt($('viaSoakTimeout').value, 10) || 800;

    viaSoakCancelled = false;
    viaSoakRunning = true;
    updateSoakButtons();
    viaBadge($('viaSoakState'), '运行中', 'warn');
    barEl.style.width = '0%';
    progEl.textContent = '0 / ' + count;
    setResult(resEl, label + ' 批量测试中（' + count + ' 次 · 间隔 ' + gapMs + 'ms · 单次超时 ' + timeoutMs + 'ms）…', '');
    viaLog('INFO', '开始批量稳定性测试：' + label + '，共 ' + count + ' 次');

    var lastPaint = 0;
    function paint(p) {
      var now = Date.now();
      if (p.done < p.total && now - lastPaint < 40) { return; }   /* 节流，避免 10000 次刷屏 */
      lastPaint = now;
      barEl.style.width = (p.done / p.total * 100).toFixed(2) + '%';
      progEl.textContent = p.done + ' / ' + p.total +
        '（成功 ' + p.ok + ' · 丢包 ' + p.loss + ' · 异常 ' + p.bad + ' · 错序 ' + p.stale + '）';
    }

    try {
      var r = await Via.soak(transport, {
        count: count,
        gapMs: gapMs,
        timeoutMs: timeoutMs,
        isCancelled: function () { return viaSoakCancelled; },
        onProgress: paint
      });
      barEl.style.width = (r.done / r.total * 100).toFixed(2) + '%';
      progEl.textContent = r.done + ' / ' + r.total +
        '（成功 ' + r.ok + ' · 丢包 ' + r.loss + ' · 异常 ' + r.bad + ' · 错序 ' + r.stale + '）';
      var rate = r.elapsedMs > 0 ? (r.done / (r.elapsedMs / 1000)) : 0;
      var lines = [
        '结果: ' + (r.cancelled ? '已停止' : '完成') + ' ' + r.done + '/' + r.total + ' 次',
        '成功: ' + r.ok + ' · 丢包: ' + r.loss + ' · 丢包率: ' + fmtPct(r.lossRate),
        '异常回包: ' + r.bad + ' · 错序/重复: ' + r.stale,
        'RTT: min ' + fmtMs(r.rtt.min) + ' · 均值 ' + fmtMs(r.rtt.mean) + ' · 中位 ' + fmtMs(r.rtt.median),
        '     P95 ' + fmtMs(r.rtt.p95) + ' · P99 ' + fmtMs(r.rtt.p99) + ' · max ' + fmtMs(r.rtt.max),
        '用时: ' + (r.elapsedMs / 1000).toFixed(2) + 's · 速率: ' + rate.toFixed(1) + ' 次/s'
      ];
      setResult(resEl, lines.join('\n'), (r.loss === 0 && r.bad === 0) ? 'ok' : 'err');
      viaLog((r.loss === 0 && r.bad === 0) ? 'OK' : 'WARN',
        label + ' 批量测试：' + r.done + ' 次，丢包 ' + r.loss + '（' + fmtPct(r.lossRate) + '），' +
        'RTT 均值 ' + fmtMs(r.rtt.mean) + ' / P95 ' + fmtMs(r.rtt.p95) + (r.cancelled ? '（已停止）' : ''));
      viaBadge($('viaSoakState'), r.cancelled ? '已停止' : '完成', (r.loss === 0 && r.bad === 0) ? 'pass' : 'fail');
    } catch (e) {
      setResult(resEl, '失败：' + e.message, 'err');
      viaLog('ERR', '批量测试失败：' + e.message);
      viaBadge($('viaSoakState'), '失败', 'fail');
    } finally {
      viaSoakRunning = false;
      updateSoakButtons();
    }
  }

  $('viaSoakRunA').addEventListener('click', function () { runSoak('A'); });
  $('viaSoakRunB').addEventListener('click', function () { runSoak('B'); });
  $('viaSoakStop').addEventListener('click', function () {
    viaSoakCancelled = true;
    viaLog('WARN', '批量测试：请求停止…');
  });

  /* ============================ LD7 键盘 VIA 数据模拟（通路 C：经串口扮演 SK32/LD7） ============================ */

  function viaSimBadge(text, cls) {
    var el = $('simState');
    el.className = 'scn-badge' + (cls ? ' ' + cls : '');
    el.textContent = text;
  }
  function kcHex(k) { return '0x' + k.toString(16).toUpperCase().padStart(4, '0'); }

  function renderSimSnapshot() {
    var el = $('simSnapshot');
    if (!viaSim) { return; }
    var s = viaSim.getSnapshot();
    var lines = [];
    lines.push('图层数: ' + s.layerCount + ' ｜ 层0: ' + s.layer0.map(kcHex).join(' '));
    lines.push('RGB: ' + (s.rgb.enabled ?
      ('mode=' + s.rgb.mode + ' bri=' + s.rgb.val + ' spd=' + s.rgb.speed + ' hue=' + s.rgb.hue + ' sat=' + s.rgb.sat) : '关闭'));
    if (s.lastSummary) { lines.push('最近: ' + s.lastSummary); }
    lines.push('隧道帧累计: ' + s.frameCount + ' ｜ VIA 请求应答: ' + s.viaCount);
    el.textContent = lines.join('\n');
  }

  function onViaSimTunnel(fr) {
    if (!commandIsSerialLink()) { return; }
    var r = viaSim.handle(fr.cmd, fr.payload);
    if (!r.handled) { return; }
    var viaLabel = (fr.cmd === F.EVT.TUNNEL_BLE) ? 'BLE' : '2.4G';
    /* 应答：CMD_TUNNEL_BLE/USB（快速路径，无 ACK，自动递增 seq、flags=0），经 sendFast 走监视器/日志 */
    link.sendFast(r.replyCmd, r.reply).catch(function (e) { viaLog('ERR', 'LD7 VIA 应答发送失败：' + e.message); });
    setResult($('simResult'), '[' + viaLabel + '] TX VIA 请求: ' + Sim.hex(fr.payload) +
      '\n→ 应答: ' + r.summary + (r.detail ? ('　' + r.detail) : '') +
      '\nRX VIA 应答: ' + Sim.hex(r.reply), 'ok');
    viaLog('OK', 'LD7 VIA [' + viaLabel + '] ' + r.summary + (r.detail ? (' ' + r.detail) : ''));
    renderSimSnapshot();
  }

  function commandIsSerialLink() {
    if (!link || !link.isConnected()) {
      setResult($('simResult'), '未连接串口（需先连接 WebSerial 到模组），无法应答隧道 VIA 请求', 'err');
      viaLog('WARN', '未连接串口，LD7 VIA 模拟无法应答');
      return false;
    }
    return true;
  }

  function viaSimToggle() {
    var btn = $('simToggle');
    if (viaSim && viaSim.running) {
      viaSim.running = false;
      btn.textContent = '启用模拟';
      viaSimBadge('未启用', '');
      setResult($('simResult'), 'LD7 VIA 模拟已停止（不再应答隧道 VIA 请求）', '');
      viaLog('INFO', 'LD7 VIA 模拟已停止');
      return;
    }
    if (!viaSim) {
      viaSim = new Sim.ViaSim({
        log: function (m) { log('INFO', m, 'SIM'); }
      });
    }
    viaSim.running = true;
    btn.textContent = '停止模拟';
    viaSimBadge('模拟运行中', 'pass');
    setResult($('simResult'), 'LD7 VIA 模拟已启用。请打开桌面 VIA（usevia.app），选择经蓝牙或 2.4G 连接的 LD7 设备进行配置', 'ok');
    viaLog('OK', 'LD7 VIA 模拟已启用（经串口扮演 SK32/LD7，协议版本 0x' + Sim.VIA_PROTOCOL_VERSION.toString(16).toUpperCase().padStart(4, '0') + '）');
    renderSimSnapshot();
  }

  $('simToggle').addEventListener('click', function () { viaSimToggle(); });
  $('simReset').addEventListener('click', function () {
    if (!viaSim) { viaSim = new Sim.ViaSim({ log: function (m) { log('INFO', m, 'SIM'); } }); }
    viaSim.resetDefaults();
    setResult($('simResult'), '已重置为 LD7 出厂默认键位 / RGB / 宏', 'ok');
    viaLog('OK', 'LD7 VIA 模拟状态已重置为出厂默认');
    renderSimSnapshot();
  });
  $('simPing').addEventListener('click', function () {
    if (!viaSim) { viaSim = new Sim.ViaSim({ log: function (m) { log('INFO', m, 'SIM'); } }); }
    /* 本地自检 GET_PROTOCOL_VERSION：不经串口，直接调用处理函数验证应答 */
    var payload = new Uint8Array(32);
    payload[0] = 0x01; /* id_get_protocol_version */
    var r = viaSim.handle(F.EVT.TUNNEL_USB, payload);
    var ok = (r.reply[0] === 0x01) && (r.reply[1] === (Sim.VIA_PROTOCOL_VERSION >> 8) & 0xFF) && (r.reply[2] === Sim.VIA_PROTOCOL_VERSION & 0xFF);
    setResult($('simResult'), '本地自检 GET_PROTOCOL_VERSION: ' + (ok ? '通过' : '未通过') +
      '\n应答: ' + Sim.hex(r.reply), ok ? 'ok' : 'err');
    viaLog(ok ? 'OK' : 'ERR', 'LD7 VIA 本地自检 ' + (ok ? 'PASS' : 'FAIL') + '：' + Sim.hex(r.reply));
  });

  /* ============================ 日志操作 ============================ */
  $('logLevelFilter').addEventListener('change', function () {
    logFilter = $('logLevelFilter').value; applyLogFilter();
  });
  $('logClear').addEventListener('click', function () { logBox.innerHTML = ''; logEntries = []; });
  $('logCopy').addEventListener('click', function () {
    var text = logEntries.map(function (e) { return e.time + ' ' + (e.dir || '  ') + ' ' + e.level + ' ' + e.text; }).join('\n');
    if (navigator.clipboard) {
      navigator.clipboard.writeText(text).then(function () { log('OK', '日志已复制到剪贴板'); },
        function (e) { log('ERR', '复制失败：' + e.message); });
    } else {
      log('WARN', '当前环境不支持剪贴板 API');
    }
  });
  $('logDownload').addEventListener('click', function () {
    var text = logEntries.map(function (e) { return e.time + ' ' + (e.dir || '  ') + ' ' + e.level + ' ' + e.text; }).join('\n');
    download('debug_log.txt', text, 'text/plain');
  });

  function download(name, text, mime) {
    var blob = new Blob([text], { type: mime || 'text/plain' });
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.href = url; a.download = name;
    document.body.appendChild(a); a.click();
    document.body.removeChild(a);
    setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
  }

  /* ============================ 启动 ============================ */
  renderCommands();
  renderScenarios();
  updateCounters();
  setHealth('', '链路：未连接');
  log('INFO', '调试工具就绪：请选择波特率（默认 921600 8N1）并连接串口（USB-TTL 直连模组 UART1）');
  log('INFO', '提示：使用本工具时须断开 SK32；OTA 升级请使用 board/ota_web');

  /* 调试出口：控制台可 DbgDebug.frame.encode(...) 等做离线核对 */
  window.DbgDebug = {
    frame: F, decode: D, link: Link, commands: Commands, hid: HID, scenarios: SCN, monitor: monitor,
    crc16: window.DbgCrc.crc16, via: Via
  };
})();
