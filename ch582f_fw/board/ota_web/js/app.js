/* OTA 升级 Web 应用 —— 流程编排（UI / 固件解析 / 状态机 / 进度 / 日志）
 *
 * 通路 B｜键盘模组：SerialOta（WebSerial，AA55 桥帧）
 * 通路 A｜接收器 Dongle：HidOta（WebHID，"OTA"+SUB，方案A：App 内收包，仅 FW_END 后复位一次）
 * 规格见同目录 SPEC.md
 */
(function () {
  'use strict';

  var F = window.OtaFrame;
  var MAX_IMAGE = 216 * 1024;

  /* ============================ DOM 工具 ============================ */

  function $(id) { return document.getElementById(id); }

  /* ============================ 日志 ============================ */

  var logBox = $('logBox');

  function ts() {
    var d = new Date();
    function p(n, w) { return String(n).padStart(w, '0'); }
    return p(d.getHours(), 2) + ':' + p(d.getMinutes(), 2) + ':' + p(d.getSeconds(), 2) +
      '.' + p(d.getMilliseconds(), 3);
  }

  function log(level, msg) {
    var line = document.createElement('div');
    var t = document.createElement('span'); t.className = 't'; t.textContent = ts();
    var l = document.createElement('span'); l.className = 'lvl ' + level; l.textContent = level;
    var m = document.createElement('span'); m.className = 'm'; m.textContent = msg;
    line.appendChild(t); line.appendChild(l); line.appendChild(m);
    logBox.appendChild(line);
    logBox.scrollTop = logBox.scrollHeight;
    while (logBox.childElementCount > 2000) { logBox.removeChild(logBox.firstChild); }
  }

  $('logClear').addEventListener('click', function () { logBox.innerHTML = ''; });
  $('logCopy').addEventListener('click', function () {
    var text = Array.prototype.map.call(logBox.children, function (el) {
      return el.children[0].textContent + ' ' + el.children[1].textContent + ' ' + el.children[2].textContent;
    }).join('\n');
    if (navigator.clipboard) {
      navigator.clipboard.writeText(text).then(function () { log('OK', '日志已复制到剪贴板'); },
        function (e) { log('ERR', '复制失败：' + e.message); });
    }
  });

  /* ============================ 兼容性检测 ============================ */

  var hasSerial = window.OtaSerial && window.OtaSerial.SerialOta.isSupported();
  var hasHid = window.OtaHid && window.OtaHid.HidOta.isSupported();

  (function initCompat() {
    var box = $('compat');
    function badge(text, cls) {
      var b = document.createElement('span');
      b.className = 'badge ' + (cls || '');
      b.textContent = text;
      box.appendChild(b);
    }
    badge('WebSerial ' + (hasSerial ? '可用' : '不可用'), hasSerial ? 'ok' : 'err');
    badge('WebHID ' + (hasHid ? '可用' : '不可用'), hasHid ? 'ok' : 'err');
    badge(window.isSecureContext ? '安全上下文' : '非安全上下文',
      window.isSecureContext ? 'ok' : 'warn');

    if (!hasSerial) { $('kbConnect').disabled = true; $('kbBaudSel').disabled = true; $('tabKb').classList.add('muted'); }
    if (!hasHid) { $('dgConnect').disabled = true; $('tabDg').classList.add('muted'); }
    if (!window.isSecureContext) {
      log('WARN', '当前不是安全上下文，WebHID/WebSerial 可能被拒绝；请用 localhost 或 file:// 打开');
    }
  })();

  /* ============================ Tab 切换 ============================ */

  var tabs = document.querySelectorAll('.tab');
  Array.prototype.forEach.call(tabs, function (tab) {
    tab.addEventListener('click', function () {
      Array.prototype.forEach.call(tabs, function (t) { t.classList.remove('active'); });
      tab.classList.add('active');
      $('panelKb').classList.add('hidden');
      $('panelDg').classList.add('hidden');
      $(tab.dataset.panel).classList.remove('hidden');
    });
  });

  /* ============================ 阶段指示 ============================ */

  var KB_STAGES = ['IDLE', 'PORT_OPEN', 'BEGIN', 'TRANSFERRING', 'END', 'DONE'];
  var DG_STAGES = ['IDLE', 'APP_OPEN', 'ENTER_OTA', 'WAIT_IAP', 'IAP_OPEN', 'BEGIN',
    'TRANSFERRING', 'END', 'WAIT_APP', 'VERIFY', 'DONE'];

  function buildStages(container, names) {
    container.innerHTML = '';
    names.forEach(function (n) {
      var s = document.createElement('span');
      s.className = 'stage';
      s.textContent = n;
      s.dataset.name = n;
      container.appendChild(s);
    });
  }
  buildStages($('kbStages'), KB_STAGES);
  buildStages($('dgStages'), DG_STAGES);

  /** state: 'active' | 'done' | 'fail' | null */
  function setStage(container, name, state) {
    var el = container.querySelector('[data-name="' + name + '"]');
    if (!el) { return; }
    el.classList.remove('active', 'done', 'fail');
    if (state) { el.classList.add(state); }
  }
  function resetStages(container) {
    Array.prototype.forEach.call(container.children, function (el) {
      el.classList.remove('active', 'done', 'fail');
    });
  }

  /* ============================ 进度 ============================ */

  function makeProgress(fillId, pctId, rateId, etaId) {
    var fill = $(fillId), pct = $(pctId), rate = $(rateId), eta = $(etaId);
    var t0 = 0, lastBytes = 0, lastT = 0;
    return {
      reset: function () {
        fill.style.width = '0%'; pct.textContent = '0%'; rate.textContent = ''; eta.textContent = '';
        t0 = 0; lastBytes = 0; lastT = 0;
      },
      update: function (done, total) {
        var now = performance.now();
        if (!t0) { t0 = now; lastT = now; lastBytes = 0; }
        var percent = total ? (done * 100 / total) : 0;
        fill.style.width = percent.toFixed(1) + '%';
        pct.textContent = percent.toFixed(1) + '%  (' + fmtBytes(done) + ' / ' + fmtBytes(total) + ')';

        var dt = (now - lastT) / 1000;
        if (dt >= 0.4) {
          var inst = (done - lastBytes) / dt;
          rate.textContent = fmtBytes(inst) + '/s';
          lastBytes = done; lastT = now;
          var avg = done / ((now - t0) / 1000);
          if (avg > 0 && done < total) {
            eta.textContent = '剩余约 ' + fmtTime((total - done) / avg);
          } else if (done >= total) {
            eta.textContent = '';
          }
        }
      },
      done: function () {
        fill.style.width = '100%'; pct.textContent = '100%';
      }
    };
  }

  function fmtBytes(n) {
    if (n < 1024) { return n.toFixed(0) + ' B'; }
    if (n < 1024 * 1024) { return (n / 1024).toFixed(1) + ' KB'; }
    return (n / 1024 / 1024).toFixed(2) + ' MB';
  }
  function fmtTime(s) {
    s = Math.max(1, Math.round(s));
    if (s < 60) { return s + ' 秒'; }
    return Math.floor(s / 60) + ' 分 ' + (s % 60) + ' 秒';
  }

  var kbProgress = makeProgress('kbFill', 'kbPct', 'kbRate', 'kbEta');
  var dgProgress = makeProgress('dgFill', 'dgPct', 'dgRate', 'dgEta');

  /* ============================ 固件加载 ============================ */

  var firmware = null;   /* { name, raw: Uint8Array, prepared: {...} } */

  function parseIntelHex(text) {
    var lines = text.split(/\r?\n/);
    var segs = [];
    var base = 0;
    for (var i = 0; i < lines.length; i++) {
      var line = lines[i].trim();
      if (!line || line.charAt(0) !== ':') { continue; }
      var hex = line.slice(1);
      var bytes = [];
      for (var j = 0; j + 1 < hex.length; j += 2) { bytes.push(parseInt(hex.substr(j, 2), 16)); }
      var len = bytes[0];
      var addr = (bytes[1] << 8) | bytes[2];
      var type = bytes[3];
      var data = bytes.slice(4, 4 + len);
      if (type === 0x00) {
        segs.push({ addr: base + addr, data: data });
      } else if (type === 0x02) {
        base = ((data[0] << 8) | data[1]) << 4;
      } else if (type === 0x04) {
        base = ((data[0] << 8) | data[1]) << 16;
      } else if (type === 0x01) {
        break;
      }
    }
    if (!segs.length) { throw new Error('HEX 文件中未找到数据记录'); }
    var lo = Infinity, hi = 0;
    segs.forEach(function (s) {
      if (s.addr < lo) { lo = s.addr; }
      if (s.addr + s.data.length > hi) { hi = s.addr + s.data.length; }
    });
    var out = new Uint8Array(hi - lo);
    out.fill(0xFF);
    segs.forEach(function (s) { out.set(s.data, s.addr - lo); });
    return out;
  }

  $('fwFile').addEventListener('change', function (ev) {
    var file = ev.target.files && ev.target.files[0];
    firmware = null;
    $('fwClear').disabled = !file;
    if (!file) {
      $('fwInfo').className = 'fw-info muted';
      $('fwInfo').textContent = '未选择文件';
      refreshStartButtons();
      return;
    }
    var reader = new FileReader();
    reader.onerror = function () {
      $('fwInfo').className = 'fw-info reject';
      $('fwInfo').textContent = '读取文件失败';
      refreshStartButtons();
    };
    reader.onload = function () {
      try {
        var raw = new Uint8Array(reader.result);
        if (/\.(hex|ihex)$/i.test(file.name)) {
          raw = parseIntelHex(new TextDecoder('utf-8').decode(raw));
          log('INFO', '已解析 Intel HEX → bin ' + raw.length + 'B');
        }
        if (raw.length > MAX_IMAGE) {
          throw new Error('镜像 ' + raw.length + 'B 超过 216KB 上限');
        }
        var prepared = window.OtaSerial.SerialOta.prepare(raw);
        firmware = { name: file.name, raw: raw, prepared: prepared };
        $('fwInfo').className = 'fw-info fine';
        $('fwInfo').textContent = file.name + '　原始 ' + prepared.rawLen + ' B → 补齐 ' + prepared.total +
          ' B　CRC 0x' + prepared.crc.toString(16).toUpperCase().padStart(4, '0') +
          '　帧数 串口 ' + Math.ceil(prepared.total / 60) + ' / HID ' + Math.ceil(prepared.total / 24);
        log('OK', '已加载固件 ' + file.name + '（' + prepared.total + 'B，CRC 0x' +
          prepared.crc.toString(16).toUpperCase().padStart(4, '0') + '）');
      } catch (e) {
        firmware = null;
        $('fwInfo').className = 'fw-info reject';
        $('fwInfo').textContent = '无法使用该文件：' + e.message;
        log('ERR', '固件加载失败：' + e.message);
      }
      refreshStartButtons();
    };
    reader.readAsArrayBuffer(file);
  });

  $('fwClear').addEventListener('click', function () {
    $('fwFile').value = '';
    firmware = null;
    $('fwClear').disabled = true;
    $('fwInfo').className = 'fw-info muted';
    $('fwInfo').textContent = '未选择文件';
    refreshStartButtons();
  });

  /* ============================ 通道 B：键盘模组（串口） ============================ */

  var kb = { ota: null, state: 'idle' };  /* idle | connected | busy */

  /** 读取连接区所选波特率（默认 921600） */
  function selectedBaud() {
    var el = $('kbBaudSel');
    var v = el ? parseInt(el.value, 10) : 921600;
    return (v > 0) ? v : 921600;
  }

  function kbSetState(s) {
    kb.state = s;
    $('kbConnect').disabled = (s === 'busy') || !hasSerial;
    $('kbConnect').textContent = (s === 'connected' || s === 'busy') ? '断开串口' : '连接串口';
    $('kbAbort').disabled = (s !== 'busy');
    $('kbBaudSel').disabled = (s === 'connected' || s === 'busy') || !hasSerial;
    refreshStartButtons();
  }

  function refreshStartButtons() {
    $('kbStart').disabled = !(firmware && kb.state === 'connected');
    $('dgStart').disabled = !(firmware && dg.state === 'connected');
  }

  $('kbConnect').addEventListener('click', async function () {
    if (kb.state === 'connected') {
      try { await kb.ota.close(); } catch (e) { /* ignore */ }
      kb.ota = null;
      $('kbDev').className = 'dev-info muted';
      $('kbDev').textContent = '未连接';
      resetStages($('kbStages'));
      setStage($('kbStages'), 'IDLE', 'active');
      kbSetState('idle');
      log('INFO', '串口已断开');
      return;
    }
    try {
      kb.ota = new window.OtaSerial.SerialOta({
        baud: selectedBaud(),
        log: log,
        onProgress: kbProgress.update,
        onDisconnect: function () {
          log('WARN', '串口断开（设备复位搬运中）');
          kbSetState('idle');
        }
      });
      var port = await kb.ota.connect();
      setStage($('kbStages'), 'IDLE', 'done');
      setStage($('kbStages'), 'PORT_OPEN', 'active');
      var info = port.getInfo ? port.getInfo() : {};
      var desc = 'COM 已打开（' + kb.ota.baud + ' 8N1）' +
        (info.usbVendorId ? '  USB ' + hex16(info.usbVendorId) + ':' + hex16(info.usbProductId) : '');
      $('kbDev').className = 'dev-info fine';
      $('kbDev').textContent = desc;
      log('OK', desc);
      kbSetState('connected');
      try {
        var v = await kb.ota.getVersion();
        log('OK', '模组版本 ' + v[0] + '.' + v[1] + '.' + v[2]);
      } catch (e) {
        log('WARN', 'GET_VERSION 未响应：' + e.message + '（请确认 USB-TTL 已直连模组 UART1）');
      }
    } catch (e) {
      kb.ota = null;
      kbSetState('idle');
      $('kbDev').className = 'dev-info';
      $('kbDev').textContent = '未连接';
      log(e && e.name === 'NotFoundError' ? 'WARN' : 'ERR',
        (e && e.name === 'NotFoundError') ? '未选择串口' : '串口打开失败：' + e.message);
    }
  });

  $('kbStart').addEventListener('click', async function () {
    if (!firmware || !kb.ota) { return; }
    var chunk = parseInt($('kbChunk').value, 10) || 60;
    kbSetState('busy');
    kbProgress.reset();
    resetStages($('kbStages'));
    setStage($('kbStages'), 'IDLE', 'done');
    setStage($('kbStages'), 'PORT_OPEN', 'done');
    setStage($('kbStages'), 'BEGIN', 'active');
    log('INFO', '======== 开始串口 OTA（分片 ' + chunk + 'B） ========');
    try {
      await kb.ota.flash(firmware.raw, { chunk: chunk });
      kbProgress.done();
      setStage($('kbStages'), 'BEGIN', 'done');
      setStage($('kbStages'), 'TRANSFERRING', 'done');
      setStage($('kbStages'), 'END', 'done');
      setStage($('kbStages'), 'DONE', 'active');
      log('OK', '======== 升级完成：模组将复位并搬运镜像 ========');
    } catch (e) {
      setStage($('kbStages'), 'DONE', 'fail');
      log('ERR', '升级失败：' + e.message);
    } finally {
      kb.ota.abortRequested = false;
      kbSetState(kb.ota.isConnected() ? 'connected' : 'idle');
    }
  });

  /* 传输中在 BEGIN/TRANSFERRING 之间切换阶段提示 */
  var kbStageTick = setInterval(function () {
    if (kb.state === 'busy') {
      var beg = $('kbStages').querySelector('[data-name="BEGIN"]');
      if (beg && beg.classList.contains('active')) {
        setStage($('kbStages'), 'BEGIN', 'done');
        setStage($('kbStages'), 'TRANSFERRING', 'active');
      }
    }
  }, 300);

  $('kbAbort').addEventListener('click', function () {
    if (kb.ota) { kb.ota.abort(); log('WARN', '已请求中止（将在当前分片后停下）'); }
  });

  /* ============================ 通道 A：接收器（HID） ============================ */

  var dg = { hid: null, state: 'idle', info: null };  /* idle | connected | busy */

  function dgSetState(s) {
    dg.state = s;
    $('dgConnect').disabled = (s === 'busy') || !hasHid;
    $('dgConnect').textContent = (s === 'connected' || s === 'busy') ? '断开设备' : '选择设备';
    $('dgAbort').disabled = (s !== 'busy');
    $('dgInfo').disabled = (s !== 'connected');
    refreshStartButtons();
  }

  function dgDescribe(info) {
    if (!info) { return '已连接（未获取到版本信息）'; }
    return window.OtaHid.modeName(info.mode) + ' 模式' +
      '　版本 ' + info.fwRev +
      '　USB ' + hex16(info.vid) + ':' + hex16(info.pid);
  }

  function hex16(n) { return '0x' + (n & 0xFFFF).toString(16).toUpperCase().padStart(4, '0'); }

  $('dgConnect').addEventListener('click', async function () {
    if (dg.state === 'connected') {
      try { await dg.hid.close(); } catch (e) { /* ignore */ }
      dg.hid = null; dg.info = null;
      $('dgDev').className = 'dev-info muted';
      $('dgDev').textContent = '未连接';
      resetStages($('dgStages'));
      setStage($('dgStages'), 'IDLE', 'active');
      dgSetState('idle');
      log('INFO', 'HID 设备已断开');
      return;
    }
    try {
      dg.hid = new window.OtaHid.HidOta({
        log: log,
        onProgress: dgProgress.update,
        onDisconnect: function () { /* 方案A 全程无中途重枚举（复位仅在 FW_END 后），此处仅记录 */ }
      });
      var dev = await dg.hid.requestDevice();
      log('INFO', '已选择设备：' + (dev.productName || '(未命名)'));
      dg.info = await dg.hid.open(dev);
      setStage($('dgStages'), 'IDLE', 'done');
      setStage($('dgStages'), 'APP_OPEN', 'active');
      if (dg.info && dg.info.mode === window.OtaHid.MODE_IAP) {
        log('WARN', '设备当前已在 IAP/OTA 模式，可直接开始升级');
        setStage($('dgStages'), 'APP_OPEN', 'done');
        setStage($('dgStages'), 'ENTER_OTA', 'done');
        setStage($('dgStages'), 'WAIT_IAP', 'done');
        setStage($('dgStages'), 'IAP_OPEN', 'active');
      }
      $('dgDev').className = 'dev-info fine';
      $('dgDev').textContent = dgDescribe(dg.info);
      log('OK', '设备已连接：' + dgDescribe(dg.info));
      dgSetState('connected');
    } catch (e) {
      dg.hid = null; dg.info = null;
      dgSetState('idle');
      var canceled = e && (e.name === 'NotFoundError' || /未选择设备/.test(e.message || ''));
      log(canceled ? 'WARN' : 'ERR', canceled ? '未选择设备' : '连接失败：' + e.message);
    }
  });

  $('dgInfo').addEventListener('click', async function () {
    try {
      var info = await dg.hid.getInfo(1500, 1);
      dg.info = info;
      $('dgDev').textContent = dgDescribe(info);
      log('OK', '设备信息：' + dgDescribe(info));
    } catch (e) {
      log('ERR', '读取设备信息失败：' + e.message);
    }
  });

  $('dgStart').addEventListener('click', async function () {
    if (!firmware || !dg.hid) { return; }
    var hid = dg.hid;
    dgSetState('busy');
    dgProgress.reset();
    resetStages($('dgStages'));
    setStage($('dgStages'), 'IDLE', 'done');
    log('INFO', '======== 开始 Dongle OTA ========');
    try {
      /* 1) 确认当前阶段。
       * 方案A：固件由 **App 直接收包**（FW_BEGIN/DATA/END 写 Stage），全程 USB 无
       * 断连，不再切到 IAP 模式。故这里不再 waitForMode(IAP)——Chrome WebHID 在
       * 设备复位重枚举后不会把设备交回 getDevices()，那条路必然超时。 */
      var info = dg.info;
      try { info = await hid.getInfo(1000, 0); } catch (e) { /* 沿用连接时结果 */ }

      setStage($('dgStages'), 'APP_OPEN', 'done');
      setStage($('dgStages'), 'ENTER_OTA', 'active');
      log('INFO', '发送 ENTER_OTA（App 直接收包，设备不复位）…');
      await hid.enterOta();
      setStage($('dgStages'), 'ENTER_OTA', 'done');
      setStage($('dgStages'), 'WAIT_IAP', 'done');   /* 无模式切换：直接略过 */
      setStage($('dgStages'), 'IAP_OPEN', 'done');
      dg.info = info;
      $('dgDev').textContent = dgDescribe(info);

      /* 2) 传输（同一连接，无中途重枚举） */
      setStage($('dgStages'), 'BEGIN', 'active');
      setStage($('dgStages'), 'TRANSFERRING', 'active');
      await hid.flash(firmware.raw);
      dgProgress.done();
      setStage($('dgStages'), 'BEGIN', 'done');
      setStage($('dgStages'), 'TRANSFERRING', 'done');
      setStage($('dgStages'), 'END', 'done');

      /* 3) FW_END 已应答即成功：固件随后写 IAP 标志 + 复位，由常驻 BOOT 把
       *    Stage 搬到 APP_A 并跳新 App。此处不再依赖静默重连（见上），
       *    避免误报「等待设备进入 App 模式超时」。 */
      setStage($('dgStages'), 'WAIT_APP', 'done');
      setStage($('dgStages'), 'VERIFY', 'done');
      setStage($('dgStages'), 'DONE', 'active');
      log('OK', '======== 升级完成（FW_END 已确认，设备将自动复位并运行新固件） ========');
      log('INFO', '如需确认新版本号，请点「选择设备」重新连接后查看');
    } catch (e) {
      setStage($('dgStages'), 'DONE', 'fail');
      log('ERR', '升级失败：' + e.message);
      log('WARN', '若设备停留在 IAP：可断电重启后重试，或用 PB22 进 ROM ISP（WCHISPTool）重烧');
    } finally {
      hid.abortRequested = false;
      dgSetState(hid.isConnected() ? 'connected' : 'idle');
    }
  });

  var dgStageTick = setInterval(function () {
    if (dg.state === 'busy') {
      var beg = $('dgStages').querySelector('[data-name="BEGIN"]');
      if (beg && beg.classList.contains('active')) {
        setStage($('dgStages'), 'BEGIN', 'done');
        setStage($('dgStages'), 'TRANSFERRING', 'active');
      }
    }
  }, 300);

  $('dgAbort').addEventListener('click', function () {
    if (dg.hid) { dg.hid.abort(); log('WARN', '已请求中止（将在当前分片后停下）'); }
  });

  /* ============================ 启动 ============================ */

  setStage($('kbStages'), 'IDLE', 'active');
  setStage($('dgStages'), 'IDLE', 'active');
  log('INFO', 'OTA 升级工具就绪；请先选择固件文件，再连接对应通道');
  if (!hasSerial && !hasHid) {
    log('ERR', '当前浏览器不支持 WebSerial / WebHID，请使用 Chrome 或 Edge');
  }

  /* 调试出口：控制台可 OtaDebug.SerialOta.prepare(...) 等做离线核对 */
  window.OtaDebug = {
    SerialOta: window.OtaSerial.SerialOta,
    HidOta: window.OtaHid.HidOta,
    crc16: window.OtaCrc.crc16,
    frame: F
  };

  void kbStageTick; void dgStageTick;
})();
