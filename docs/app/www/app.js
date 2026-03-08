/* ── Globals ───────────────────────────────────────────────────── */
var currentVersion = null;
var deviceConfig = null;   /* config from device */
var pendingConfig = null;  /* edited but not rebooted */
var missCount = 0;
var showAllVersions = false;

var GITHUB_REPO = 'n9wxu/pyro_fw';
var ASSET_NAME = 'pyro_fw_c_fota_image.bin';
var MAX_ALT = {0:800000, 1:8000, 2:26247};
var UNIT_LABELS = {0:'cm', 1:'m', 2:'ft'};
var UNIT_NAMES = ['cm','m','ft'];
var MODE_LABELS = {delay:'Delay',agl:'AGL',fallen:'Fallen',speed:'Speed',none:'None'};
var WEB_VERSION = '2.0.0';

/* ── Tabs ──────────────────────────────────────────────────────── */
function showTab(name) {
  document.querySelectorAll('.tabpanel').forEach(function(el) { el.style.display = 'none'; });
  document.querySelectorAll('.tab').forEach(function(el) { el.classList.remove('active'); });
  document.getElementById('tab-' + name).style.display = 'block';
  event.target.classList.add('active');
  if (name === 'data') { loadFlightData(); drawGraph(); }
}

/* ── Unit conversion ───────────────────────────────────────────── */
function cmToUnit(cm, u) {
  if (u === 1) return (cm / 100).toFixed(1);
  if (u === 2) return (cm / 30.48).toFixed(1);
  return cm;
}
function cmsToUnit(cms, u) {
  if (u === 1) return (cms / 100).toFixed(1);
  if (u === 2) return (cms / 30.48).toFixed(1);
  return cms;
}
function unitLabel(u) { return UNIT_LABELS[u] || 'cm'; }
function fmtMode(mode, val, u) {
  if (mode === 'delay') return 'Delay ' + val + 's';
  return (MODE_LABELS[mode]||mode) + ' ' + val + ' ' + unitLabel(u);
}

/* ── Status polling ────────────────────────────────────────────── */
function update() {
  fetch('api/status').then(function(r){return r.json()}).then(function(d) {
    missCount = 0;
    var u = d.units || 0;
    var ul = unitLabel(u);

    document.getElementById('sState').textContent = d.state;
    document.getElementById('sAlt').textContent = cmToUnit(d.alt_cm, u) + ' ' + ul;
    document.getElementById('sMax').textContent = cmToUnit(d.max_alt_cm, u) + ' ' + ul;
    document.getElementById('sSpd').textContent = cmsToUnit(d.vspeed_cms, u) + ' ' + ul + '/s';
    document.getElementById('sPa').textContent = d.pressure_pa + ' Pa';
    document.getElementById('sFt').textContent = (d.flight_ms/1000).toFixed(1) + 's';
    document.getElementById('sUp').textContent = (d.uptime/1000).toFixed(0) + 's';

    /* Pyro status */
    function pyroStr(fired, cont, adc) {
      if (fired) return '<span class="pyro-fired">FIRED</span> (ADC:' + adc + ')';
      if (cont) return '<span class="pyro-ok">OK</span> (ADC:' + adc + ')';
      return '<span class="pyro-open">OPEN</span> (ADC:' + adc + ')';
    }
    document.getElementById('sP1').innerHTML = pyroStr(d.pyro1_fired, d.pyro1_cont, d.pyro1_adc);
    document.getElementById('sP2').innerHTML = pyroStr(d.pyro2_fired, d.pyro2_cont, d.pyro2_adc);
    document.getElementById('sArm').textContent = d.armed ? 'YES' : 'No';

    /* Config display */
    document.getElementById('sCfgId').textContent = d.rocket_id || '—';
    document.getElementById('sCfgName').textContent = d.rocket_name || '—';
    document.getElementById('sCfgUnits').textContent = UNIT_NAMES[u] || 'cm';
    var p1Str = fmtMode(d.pyro1_mode, d.pyro1_value, u);
    var p2Str = fmtMode(d.pyro2_mode, d.pyro2_value, u);
    if (pendingConfig) {
      p1Str = fmtMode(pendingConfig.p1mode, pendingConfig.p1val, pendingConfig.units);
      p2Str = fmtMode(pendingConfig.p2mode, pendingConfig.p2val, pendingConfig.units);
    }
    document.getElementById('sCfgP1').innerHTML = p1Str + (pendingConfig ? ' <span class="warn-inline">not yet applied</span>' : '');
    document.getElementById('sCfgP2').innerHTML = p2Str + (pendingConfig ? ' <span class="warn-inline">not yet applied</span>' : '');
    document.getElementById('pendingWarn').style.display = pendingConfig ? 'block' : 'none';

    /* Flight summary */
    document.getElementById('dDur').textContent = (d.flight_ms/1000).toFixed(1) + 's';
    document.getElementById('dApogee').textContent = cmToUnit(d.max_alt_cm, u) + ' ' + ul;
    var p1Txt = 'Not fired';
    if (d.pyro1_mode === 'none') p1Txt = 'Disabled';
    else if (d.pyro1_fired) p1Txt = 'Fired';
    var p2Txt = 'Not fired';
    if (d.pyro2_mode === 'none') p2Txt = 'Disabled';
    else if (d.pyro2_fired) p2Txt = 'Fired';
    document.getElementById('dP1').innerHTML = p1Txt;
    document.getElementById('dP2').innerHTML = p2Txt;

    /* Version info */
    currentVersion = d.fw_version;
    document.getElementById('uFwVer').textContent = d.fw_version;
    document.getElementById('uWebVer').textContent = WEB_VERSION;

    /* Store device config — update every poll */
    var newCfg = {id:d.rocket_id, name:d.rocket_name, units:u, beep:'digits',
      p1mode:d.pyro1_mode, p1val:d.pyro1_value, p2mode:d.pyro2_mode, p2val:d.pyro2_value};
    if (!deviceConfig) {
      deviceConfig = newCfg;
      cfgLoadFromObj(deviceConfig);
    } else {
      /* Detect device-side change (reboot applied new config) */
      if (deviceConfig.p1mode !== newCfg.p1mode || deviceConfig.p1val !== newCfg.p1val ||
          deviceConfig.p2mode !== newCfg.p2mode || deviceConfig.p2val !== newCfg.p2val ||
          deviceConfig.units !== newCfg.units) {
        deviceConfig = newCfg;
        pendingConfig = null;
        cfgLoadFromObj(deviceConfig);
      }
    }
  }).catch(function() {
    if (++missCount > 3) document.getElementById('sState').textContent = 'Connection lost';
  });
  setTimeout(update, 1000);
}

/* ── Config editor ─────────────────────────────────────────────── */
function getUnits() { return parseInt(document.getElementById('cfgUnits').value); }
function getMaxAlt() { return MAX_ALT[getUnits()]; }

function cfgChanged() {
  /* Update unit labels */
  [1,2].forEach(function(ch) {
    var mode = document.getElementById('p'+ch+'mode').value;
    var uSpan = document.getElementById('p'+ch+'unit');
    var vInput = document.getElementById('p'+ch+'val');
    if (mode === 'none') { uSpan.textContent = ''; vInput.disabled = true; vInput.value = 0; }
    else if (mode === 'delay') { uSpan.textContent = 'seconds'; vInput.max = 65535; vInput.disabled = false; }
    else if (mode === 'speed') { uSpan.textContent = unitLabel(getUnits()) + '/s'; vInput.max = getMaxAlt(); vInput.disabled = false; }
    else { uSpan.textContent = unitLabel(getUnits()); vInput.max = getMaxAlt(); vInput.disabled = false; }
    /* Range warning */
    var val = parseInt(vInput.value) || 0;
    var warn = document.getElementById('p'+ch+'warn');
    warn.textContent = (mode !== 'delay' && val > getMaxAlt()) ?
      '⚠ Exceeds ' + getMaxAlt() + ' ' + unitLabel(getUnits()) + ' sensor limit' : '';
  });
  /* Tips */
  var tips = document.getElementById('cfgTips');
  var p1 = document.getElementById('p1mode').value;
  var p2 = document.getElementById('p2mode').value;
  var msgs = [];
  if (p1 === 'delay' && document.getElementById('p1val').value === '0')
    msgs.push('💡 Delay=0 fires at apogee (typical for drogue)');
  if (p2 === 'agl') {
    var v = parseInt(document.getElementById('p2val').value) || 0;
    var u = getUnits();
    var low = u===2?200:u===1?60:6000, high = u===2?1000:u===1?300:30000;
    if (v > 0 && v < low) msgs.push('⚠ AGL very low — main may deploy close to ground');
    if (v > high) msgs.push('⚠ AGL high — main deploys early, long descent');
  }
  if (p1 === p2 && p1 !== 'delay') msgs.push('💡 Same mode on both — consider different modes for redundancy');
  tips.style.display = msgs.length ? 'block' : 'none';
  tips.innerHTML = msgs.join('<br>');
  /* Dirty indicator */
  document.getElementById('cfgDirty').style.display = 'block';
}

function cfgLoadFromObj(c) {
  document.getElementById('cfgId').value = c.id || '';
  document.getElementById('cfgName').value = c.name || '';
  document.getElementById('cfgUnits').value = c.units || 0;
  document.getElementById('cfgBeep').value = c.beep || 'digits';
  document.getElementById('p1mode').value = c.p1mode || 'delay';
  document.getElementById('p1val').value = c.p1val || 0;
  document.getElementById('p2mode').value = c.p2mode || 'agl';
  document.getElementById('p2val').value = c.p2val || 0;
  document.getElementById('cfgDirty').style.display = 'none';
  cfgChanged();
  document.getElementById('cfgDirty').style.display = 'none';
}

function cfgDefault() {
  cfgLoadFromObj({id:'PYRO001', name:'My Rocke', units:1, beep:'digits', p1mode:'delay', p1val:0, p2mode:'agl', p2val:300});
  document.getElementById('cfgDirty').style.display = 'block';
  document.getElementById('cfgDirty').innerHTML = '⚠ Defaults loaded — press <b>Save</b> then <b>Reboot</b> to apply';
}

function cfgCurrent() {
  if (deviceConfig) cfgLoadFromObj(deviceConfig);
}

function cfgGetObj() {
  return {
    id: document.getElementById('cfgId').value,
    name: document.getElementById('cfgName').value,
    units: getUnits(),
    beep: document.getElementById('cfgBeep').value,
    p1mode: document.getElementById('p1mode').value,
    p1val: parseInt(document.getElementById('p1val').value) || 0,
    p2mode: document.getElementById('p2mode').value,
    p2val: parseInt(document.getElementById('p2val').value) || 0
  };
}

function cfgSave() {
  var c = cfgGetObj();
  var uname = UNIT_NAMES[c.units];
  var ini = '[pyro]\r\nid=' + c.id + '\r\nname=' + c.name +
    '\r\npyro1_mode=' + c.p1mode + '\r\npyro1_value=' + c.p1val +
    '\r\npyro2_mode=' + c.p2mode + '\r\npyro2_value=' + c.p2val +
    '\r\nunits=' + uname + '\r\nbeep_mode=' + c.beep + '\r\n';
  var msg = document.getElementById('cfgMsg');
  fetch('api/config', {method:'POST', headers:{'Content-Type':'text/plain'}, body:ini})
    .then(function(r) {
      if (r.ok) {
        pendingConfig = c;
        msg.style.color = 'green';
        msg.textContent = ' Saved — reboot to apply';
        document.getElementById('cfgDirty').style.display = 'block';
      } else {
        msg.style.color = 'red';
        msg.textContent = ' Error saving';
      }
    });
}

function cfgUpload() { document.getElementById('cfgFile').click(); }
function cfgFileSelected() {
  var file = document.getElementById('cfgFile').files[0];
  if (!file) return;
  var msg = document.getElementById('cfgMsg');
  file.text().then(function(txt) {
    fetch('api/config', {method:'POST', headers:{'Content-Type':'text/plain'}, body:txt})
      .then(function(r) {
        msg.style.color = r.ok ? 'green' : 'red';
        msg.textContent = r.ok ? ' Uploaded — reboot to apply' : ' Error';
        if (r.ok) { pendingConfig = cfgGetObj(); document.getElementById('cfgDirty').style.display = 'block'; }
      });
  });
}

function cfgReboot() {
  if (!confirm('Reboot device? Config changes will be applied.')) return;
  var msg = document.getElementById('cfgMsg');
  msg.style.color = 'orange';
  msg.textContent = ' Rebooting...';
  pendingConfig = null;
  deviceConfig = null;
  fetch('api/reboot', {method:'POST'}).catch(function(){});
  waitForReboot(msg);
}

/* ── Flight data ───────────────────────────────────────────────── */
function dlFlight() { window.location = 'api/flight.csv'; }

var flightData = [];
var flightEvents = {};
var flightLoaded = false;

function loadFlightData() {
  if (flightLoaded) return;
  fetch('api/flight.csv').then(function(r){return r.text()}).then(function(csv) {
    flightData = [];
    flightEvents = {};
    csv.split('\n').forEach(function(line) {
      if (!line || line.startsWith('time')) return;
      var parts = line.split(',');
      if (parts.length < 4) return;
      var t = parseInt(parts[0]), alt = parseInt(parts[2]), evt = (parts[4]||'').trim();
      if (!isNaN(t) && !isNaN(alt)) flightData.push({t:t, a:alt});
      if (evt) flightEvents[evt] = {t:t, alt:alt};
    });
    flightLoaded = true;
    updateFlightEvents();
    drawGraph();
  }).catch(function(){});
}

function updateFlightEvents() {
  var u = deviceConfig ? deviceConfig.units : 0;
  var ul = unitLabel(u);
  var p1 = document.getElementById('dP1');
  var p2 = document.getElementById('dP2');
  if (flightEvents.PYRO1) {
    p1.innerHTML = 'Fired at ' + (flightEvents.PYRO1.t/1000).toFixed(1) + 's, ' +
      cmToUnit(flightEvents.PYRO1.alt, u) + ' ' + ul;
  }
  if (flightEvents.PYRO2) {
    p2.innerHTML = 'Fired at ' + (flightEvents.PYRO2.t/1000).toFixed(1) + 's, ' +
      cmToUnit(flightEvents.PYRO2.alt, u) + ' ' + ul;
  }
}
function drawGraph() {
  var canvas = document.getElementById('flightGraph');
  var ctx = canvas.getContext('2d');
  var W = canvas.width, H = canvas.height;
  ctx.clearRect(0, 0, W, H);

  if (flightData.length < 2) {
    ctx.fillStyle = '#999';
    ctx.font = '14px sans-serif';
    ctx.fillText('No flight data available', W/2 - 80, H/2);
    return;
  }

  var maxT = flightData[flightData.length-1].t;
  var maxA = 0;
  flightData.forEach(function(p) { if (p.a > maxA) maxA = p.a; });
  if (maxA === 0) maxA = 100;
  var pad = {l:50, r:10, t:10, b:30};
  var gw = W - pad.l - pad.r, gh = H - pad.t - pad.b;

  /* Grid */
  ctx.strokeStyle = '#ddd'; ctx.lineWidth = 1;
  for (var i = 0; i <= 4; i++) {
    var y = pad.t + gh - (i/4)*gh;
    ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(pad.l+gw, y); ctx.stroke();
  }

  /* Axes labels */
  ctx.fillStyle = '#666'; ctx.font = '11px sans-serif';
  var u = deviceConfig ? deviceConfig.units : 0;
  for (var i = 0; i <= 4; i++) {
    var y = pad.t + gh - (i/4)*gh;
    ctx.fillText(cmToUnit(maxA * i/4, u), 2, y + 4);
  }
  for (var i = 0; i <= 5; i++) {
    var x = pad.l + (i/5)*gw;
    ctx.fillText((maxT * i/5 / 1000).toFixed(0) + 's', x - 8, H - 5);
  }

  /* Altitude line */
  ctx.strokeStyle = '#2266cc'; ctx.lineWidth = 2;
  ctx.beginPath();
  flightData.forEach(function(p, idx) {
    var x = pad.l + (p.t / maxT) * gw;
    var y = pad.t + gh - (p.a / maxA) * gh;
    if (idx === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.stroke();
}

/* ── Firmware update ───────────────────────────────────────────── */
function checkUpdate() {
  var msg = document.getElementById('updMsg');
  msg.style.color = 'orange'; msg.textContent = ' Checking...';
  if (!currentVersion) { msg.textContent = ' Waiting for device...'; return; }
  fetch('https://api.github.com/repos/' + GITHUB_REPO + '/releases/latest')
    .then(function(r) { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
    .then(function(rel) {
      var ver = rel.tag_name.replace(/^v/, '');
      if (ver === currentVersion) {
        msg.style.color = 'green'; msg.textContent = ' Up to date (v' + ver + ')';
      } else {
        var asset = rel.assets.find(function(a){return a.name === ASSET_NAME});
        if (!asset) throw new Error(ASSET_NAME + ' not found');
        msg.style.color = 'blue';
        msg.innerHTML = ' v' + currentVersion + ' → v' + ver +
          ' <a href="' + asset.browser_download_url + '">⬇ Download</a> then Upload below';
      }
    }).catch(function(e) { msg.style.color = 'red'; msg.textContent = ' ' + e.message; });
}

function toggleAllVersions() {
  showAllVersions = !showAllVersions;
  var btn = document.getElementById('btnAllVer');
  var div = document.getElementById('versionList');
  if (!showAllVersions) { div.style.display = 'none'; btn.textContent = '📋 Show All Versions'; return; }
  btn.textContent = '📋 Hide Versions';
  div.style.display = 'block';
  div.innerHTML = '<em>Loading...</em>';
  fetch('https://api.github.com/repos/' + GITHUB_REPO + '/releases')
    .then(function(r){return r.json()})
    .then(function(releases) {
      var html = '<table><tr><th>Version</th><th>Date</th><th>Type</th><th></th></tr>';
      releases.forEach(function(rel) {
        var ver = rel.tag_name.replace(/^v/, '');
        var date = rel.published_at ? rel.published_at.substring(0,10) : '';
        var type = rel.prerelease ? 'beta' : 'release';
        var isCurrent = (ver === currentVersion);
        var asset = rel.assets.find(function(a){return a.name === ASSET_NAME});
        var dl = asset ? '<a href="' + asset.browser_download_url + '">⬇</a>' : '—';
        html += '<tr class="' + (isCurrent?'current':'') + '"><td>v' + ver + '</td><td>' + date +
          '</td><td>' + type + '</td><td>' + (isCurrent ? '✓ current' : dl) + '</td></tr>';
      });
      html += '</table>';
      div.innerHTML = html;
    }).catch(function(e) { div.innerHTML = '<span style="color:red">' + e.message + '</span>'; });
}

function uploadFW() {
  var file = document.getElementById('fwfile').files[0];
  if (!file) { alert('Select a .bin file'); return; }
  if (!confirm('Flash firmware? Device will reboot.')) return;
  var msg = document.getElementById('fwmsg');
  msg.style.color = 'orange'; msg.textContent = ' Uploading...';
  file.arrayBuffer().then(function(buf) {
    fetch('api/ota', {method:'POST', body:new Uint8Array(buf)})
      .then(function() { msg.textContent = ' Rebooting...'; deviceConfig = null; waitForReboot(msg); })
      .catch(function() { msg.textContent = ' Rebooting...'; deviceConfig = null; waitForReboot(msg); });
  });
}

function uploadWeb() {
  var file = document.getElementById('webfile').files[0];
  var path = document.getElementById('webpath').value;
  if (!file || !path) { alert('Select file and enter path'); return; }
  var msg = document.getElementById('webmsg');
  file.arrayBuffer().then(function(buf) {
    fetch(path, {method:'POST', body:new Uint8Array(buf)})
      .then(function(r) { msg.style.color = r.ok?'green':'red'; msg.textContent = r.ok?' Uploaded':' Error'; });
  });
}

function waitForReboot(msg) {
  var attempts = 0;
  var poll = setInterval(function() {
    if (++attempts > 30) { clearInterval(poll); msg.style.color='red'; msg.textContent=' Device not responding'; return; }
    fetch('api/status').then(function(r){return r.json()}).then(function(d) {
      clearInterval(poll);
      msg.style.color = 'green';
      msg.textContent = ' Online — v' + d.fw_version;
      currentVersion = d.fw_version;
      pendingConfig = null;
      deviceConfig = null;
      document.getElementById('cfgDirty').style.display = 'none';
      document.getElementById('pendingWarn').style.display = 'none';
    }).catch(function(){});
  }, 2000);
}

/* ── Init ──────────────────────────────────────────────────────── */
update();
