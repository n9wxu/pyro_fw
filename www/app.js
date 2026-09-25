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
/* beep_mode is a U8 in config_fields.h, so sending the name made atoi() read 0
 * on every save. Nothing in the firmware consumes the field yet. */
var BEEP_CODES = {digits:0, hundreds:1};
function beepCode(n) { return BEEP_CODES[n] !== undefined ? BEEP_CODES[n] : 0; }
var WEB_VERSION = '2.0.0';

/* ── Tabs ──────────────────────────────────────────────────────── */
function showTab(name) {
  document.querySelectorAll('.tabpanel').forEach(function(el) { el.style.display = 'none'; });
  document.querySelectorAll('.tab').forEach(function(el) { el.classList.remove('active'); });
  document.getElementById('tab-' + name).style.display = 'block';
  event.target.classList.add('active');
  if (name === 'data') { loadFlightData(); drawGraph(); }
  if (name === 'lua') { luaInit(); }
  if (name === 'config') { relInit(); }
  if (name === 'beeps') { beepsInit(); }
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
  fetch('/api/status').then(function(r){return r.json()}).then(function(d) {
    missCount = 0;
    var u = d.units || 0;
    var ul = unitLabel(u);

    if (d.board) {
      document.getElementById('sBoard').textContent = d.board;
      document.title = d.board;
    }
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
    '\r\nunits=' + uname + '\r\nbeep_mode=' + beepCode(c.beep) + '\r\n';
  var msg = document.getElementById('cfgMsg');
  fetch('/api/config', {method:'POST', headers:{'Content-Type':'text/plain'}, body:ini})
    .then(function(r) {
      if (r.ok) {
        return r.json().then(function(data) {
          if (data.applied) {
            msg.style.color = 'green';
            msg.textContent = ' ✓ Config applied successfully!';
            deviceConfig = c;
            pendingConfig = null;
            document.getElementById('cfgDirty').style.display = 'none';
          } else {
            msg.style.color = 'orange';
            msg.textContent = ' Saved — reboot to apply';
            pendingConfig = c;
            document.getElementById('cfgDirty').style.display = 'block';
          }
        });
      } else {
        return r.json().catch(function() { return {error: 'Save failed'}; });
      }
    })
    .then(function(err) {
      if (err) {
        msg.style.color = 'red';
        msg.textContent = ' ' + (err.error || 'Error saving');
      }
    })
    .catch(function() {
      msg.style.color = 'red';
      msg.textContent = ' Connection error';
    });
}

function cfgUpload() { document.getElementById('cfgFile').click(); }
function cfgFileSelected() {
  var file = document.getElementById('cfgFile').files[0];
  if (!file) return;
  var msg = document.getElementById('cfgMsg');
  file.text().then(function(txt) {
    fetch('/api/config', {method:'POST', headers:{'Content-Type':'text/plain'}, body:txt})
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
  fetch('/api/reboot', {method:'POST'}).catch(function(){});
  waitForReboot(msg);
}

/* ── Flight data ───────────────────────────────────────────────── */
function dlFlight() { window.location = '/api/flight.csv'; }

var flightData = [];
var flightEvents = {};
var flightLoaded = false;

function loadFlightData() {
  if (flightLoaded) return;
  fetch('/api/flight.csv').then(function(r){return r.text()}).then(function(csv) {
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
    fetch('/api/ota', {method:'POST', body:new Uint8Array(buf)})
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
    fetch('/api/status').then(function(r){return r.json()}).then(function(d) {
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


/* ── Lua ───────────────────────────────────────────────────────── */

/* ── Beep personalities ────────────────────────────────────────
 *
 * Everything here comes from /api/beeps: the outcomes, their meanings, the
 * pattern kinds and the three personalities. The firmware is the only place
 * the vocabulary is written down, so adding an outcome or a kind grows the
 * table without touching this file.
 *
 * Defaults follow Eggtimer Rocketry, whose convention most fliers already
 * know: a rapid chirp means ready, a repeating beep count means a fault. */
var beepCaps = null;
var beepEdit = null;   /* the personality being edited, as a working copy */
var beepsReady = false;

var KIND_LABEL = {silent: 'silent', chirp: 'chirp (ready)', tone: 'steady tone', code: 'beep count'};
function kindLabel(k) { return KIND_LABEL[k] || k; }

function beepsInit() {
  if (beepsReady) return;
  beepsReady = true;
  beepsFetch().then(renderBeeps).catch(function(e) {
    document.getElementById('bpHint').textContent = 'could not read the beep table: ' + e.message;
  });
}

function beepsFetch() {
  return fetch('/api/beeps')
    .then(function(r) { if (!r.ok) throw new Error('beeps ' + r.status); return r.json(); })
    .then(function(d) { beepCaps = d; beepEdit = JSON.parse(JSON.stringify(d.personalities)); return d; });
}

function renderBeeps() {
  var sel = document.getElementById('bpSel');
  sel.innerHTML = '';
  beepEdit.forEach(function(p, i) {
    var o = document.createElement('option');
    o.value = i; o.textContent = p.name || ('Slot ' + i);
    sel.appendChild(o);
  });
  sel.value = beepCaps.active;
  document.getElementById('bpHint').innerHTML =
    (beepCaps.has_buzzer ? '' : '<b>This board has no buzzer fitted</b>, so nothing here can be heard on it. ') +
    (beepCaps.reason ? '<b>' + esc(beepCaps.reason) + '</b>' : '');
  beepsSelect();
}

/* Switch to a personality, or re-render the current one after a change that
   alters which rows apply. */
function beepsSelect(keep) {
  var i = +document.getElementById('bpSel').value;
  var p = beepEdit[i];
  if (!keep) {
    document.getElementById('bpName').value = p.name;
    document.getElementById('bpGap').value = p.gap;
    document.getElementById('bpRepeat').value = p.repeat;
    document.getElementById('bpSplit').checked = p.split;
  }
  p.split = document.getElementById('bpSplit').checked;

  var html = '<tr><th>Sounds like</th><th></th><th></th><th>Means</th></tr>';
  beepCaps.outcomes.forEach(function(o) {
    /* With the channels merged, channel 2 is never played, so offering a
       sound for it would be offering something the board cannot say. */
    if (o.key === 'check_pyro_2' && !p.split) return;
    var sp = p.spec[o.key];
    var k = esc(o.key);
    html += '<tr id="brow' + k + '"><td class="lbl">' +
            '<select id="bk' + k + '" onchange="beepsCheck()">';
    beepCaps.kinds.forEach(function(kind) {
      html += '<option value="' + esc(kind) + '"' + (kind === sp.kind ? ' selected' : '') +
              '>' + esc(kindLabel(kind)) + '</option>';
    });
    html += '</select></td><td>' +
            '<input id="bd1' + k + '" type="number" min="' + beepCaps.digit_min + '" max="' + beepCaps.digit_max +
            '" value="' + (sp.d1 || 1) + '" style="width:3.2em" oninput="beepsCheck()">' +
            '<input id="bd2' + k + '" type="number" min="0" max="' + beepCaps.digit_max +
            '" value="' + sp.d2 + '" style="width:3.2em" oninput="beepsCheck()" title="0 = one group">' +
            '</td><td>' + (beepCaps.has_buzzer
              ? '<button onclick="beepsPlay(\'' + k + '\')" title="Play this row">▶</button>' : '') +
            '</td><td class="val">' + esc(o.what) + '</td></tr>';
  });
  document.getElementById('bpTable').innerHTML = html;
  beepsCheck();
}

/* Read the form back into the working copy, and flag the two mistakes that
   are easy to make. The firmware validates and is authoritative. */
function beepsCheck() {
  if (!beepCaps) return;
  var i = +document.getElementById('bpSel').value;
  var p = beepEdit[i];
  p.name = document.getElementById('bpName').value;
  p.gap = parseInt(document.getElementById('bpGap').value, 10) || 0;
  p.repeat = parseInt(document.getElementById('bpRepeat').value, 10) || 0;
  p.split = document.getElementById('bpSplit').checked;

  var seen = {}, dupe = null, bad = null, audible = 0;
  beepCaps.outcomes.forEach(function(o) {
    var ke = document.getElementById('bk' + o.key);
    if (!ke) return;               /* hidden because the channels are merged */
    var sp = p.spec[o.key];
    sp.kind = ke.value;
    sp.d1 = parseInt(document.getElementById('bd1' + o.key).value, 10) || 0;
    sp.d2 = parseInt(document.getElementById('bd2' + o.key).value, 10) || 0;

    /* The counts only mean anything for a beep count. */
    var isCode = sp.kind === 'code';
    document.getElementById('bd1' + o.key).style.visibility = isCode ? '' : 'hidden';
    document.getElementById('bd2' + o.key).style.visibility = isCode ? '' : 'hidden';

    if (isCode && (sp.d1 < beepCaps.digit_min || sp.d1 > beepCaps.digit_max || sp.d2 > beepCaps.digit_max)) {
      bad = o.key;
      return;
    }
    if (sp.kind === 'silent') return;
    audible++;
    var sig = isCode ? 'code:' + sp.d1 + '-' + sp.d2 : sp.kind;
    if (seen[sig]) dupe = sig; else seen[sig] = o.key;
  });

  var msgs = [];
  if (bad) {
    msgs.push('<div class="warn">A beep count must be ' + beepCaps.digit_min + ' to ' + beepCaps.digit_max +
              '. A zero cannot be heard.</div>');
  }
  if (dupe) {
    msgs.push('<div class="warn">Two outcomes sound the same. An operator hearing it would get the wrong ' +
              'answer half the time.</div>');
  }
  if (!audible) {
    msgs.push('<div class="warn">This personality says nothing at all, which tells an operator nothing.</div>');
  }
  document.getElementById('bpWarn').innerHTML = msgs.join('');

  /* Keep the menu label in step with the name box. */
  var opt = document.getElementById('bpSel').options[i];
  if (opt) opt.textContent = p.name || ('Slot ' + i);
}

/* Hear it before committing to it. The board plays what is in the row,
   including an unsaved change. */
function beepsPlay(key) {
  var i = +document.getElementById('bpSel').value;
  var sp = beepEdit[i].spec[key];
  var msg = document.getElementById('bpMsg');
  fetch('/api/beeps/play', {method:'POST', headers:{'Content-Type':'text/plain'},
        body: JSON.stringify({kind: sp.kind, d1: sp.d1, d2: sp.d2})})
    .then(function(r) { return r.json().catch(function(){ return {error:'HTTP ' + r.status}; }); })
    .then(function(d) {
      msg.style.color = d.error ? 'red' : '';
      msg.textContent = d.error ? ' ✗ ' + d.error
        : ' ♪ ' + (d.kind === 'code' ? d.d1 + (d.d2 ? '–' + d.d2 : '') + ' beeps' : d.kind);
    })
    .catch(function(e) { msg.style.color = 'red'; msg.textContent = ' ✗ ' + e.message; });
}

function specToIni(sp) {
  if (sp.kind !== 'code') return sp.kind;
  return 'code:' + sp.d1 + (sp.d2 ? '-' + sp.d2 : '');
}

function beepsSave() {
  var active = +document.getElementById('bpSel').value;
  var ini = '[beeps]\r\nactive=' + active + '\r\n';
  beepEdit.forEach(function(p, i) {
    ini += 'p' + i + '_name=' + p.name + '\r\n';
    ini += 'p' + i + '_gap=' + p.gap + '\r\n';
    ini += 'p' + i + '_repeat=' + p.repeat + '\r\n';
    ini += 'p' + i + '_split=' + (p.split ? 'true' : 'false') + '\r\n';
    beepCaps.outcomes.forEach(function(o) {
      ini += 'p' + i + '_' + o.key + '=' + specToIni(p.spec[o.key]) + '\r\n';
    });
  });

  var msg = document.getElementById('bpMsg');
  msg.style.color = ''; msg.textContent = ' saving…';
  fetch('/api/beeps', {method:'POST', headers:{'Content-Type':'text/plain'}, body: ini})
    .then(function(r) { return r.json().catch(function(){ return {error:'HTTP ' + r.status}; }); })
    .then(function(d) {
      if (d.error) {
        msg.style.color = 'red';
        msg.textContent = ' ✗ ' + d.error +
          (d.personality !== undefined && d.personality >= 0 ? ' in ' + beepEdit[d.personality].name : '') +
          (d.reason ? ' (' + d.reason + ')' : '');
        return;
      }
      msg.style.color = 'green';
      msg.textContent = ' ✓ saved';
      return beepsFetch().then(renderBeeps);
    })
    .catch(function(e) { msg.style.color = 'red'; msg.textContent = ' ✗ ' + e.message; });
}

/* Put the Eggtimer convention back into the personality being edited. It is
   slot 0's shipped content, which the firmware sends; the operator still has
   to press Save. */
function beepsDefaults() {
  if (!beepCaps) return;
  var i = +document.getElementById('bpSel').value;
  beepEdit[i] = JSON.parse(JSON.stringify(beepCaps.personalities[0]));
  beepEdit[i].name = document.getElementById('bpName').value || beepEdit[i].name;
  beepsSelect();
  var msg = document.getElementById('bpMsg');
  msg.style.color = '';
  msg.textContent = ' Eggtimer defaults loaded — press Save to apply';
}

/* ── Pin capabilities ──────────────────────────────────────────
 *
 * Everything board-specific this page knows comes from /api/pins/caps. It
 * used to hardcode MK1C's four J3 pads and keep its own role list, so on
 * MK1A and MK1B the Lua tab rendered four pads that are not there and offered
 * roles no pin on those boards can take.
 *
 * The rule for "may this pin take this role" is the firmware's own -- a
 * capability bit, tested against the same mask pin_assign_validate() uses --
 * applied to the firmware's own table. Nothing here needs changing when a
 * board, a role or a capability is added. */
var pinCaps = null;

/* Presentation only, with a fallback: a role this page has not been taught
 * shows under its firmware name rather than disappearing from the menu. */
var ROLE_LABEL = {
  off: 'unused', out: 'digital out', pwm: 'dimmable out', in: 'digital in',
  tx: 'serial TX', rx: 'serial RX', pixel: 'LED string', bridge: 'half-bridge'
};
function roleLabel(r) { return ROLE_LABEL[r] || r; }

function esc(s) {
  return String(s === undefined || s === null ? '' : s)
    .replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function pinsFetch() {
  return fetch('/api/pins/caps')
    .then(function(r) { if (!r.ok) throw new Error('caps ' + r.status); return r.json(); })
    .then(function(d) { pinCaps = d; return d; });
}

function rolesFor(pin) {
  return pinCaps.roles.filter(function(r) {
    if (r.needs !== 0 && (pin.f & r.needs) === 0) return false;
    /* A pad can carry FN_BRIDGE while the board has no second half to pair it
       with -- the capability is per pin, the pairing is per board. Offering
       the role there would be offering something validation must refuse. */
    if (r.r === 'bridge' && !pinCaps.bridge_possible) return false;
    return true;
  });
}

/* Every bit any role requires: a pin with none of them is not assignable to
 * Lua at all and does not belong in the table. */
function luaAnyMask() {
  var m = 0;
  pinCaps.roles.forEach(function(r) { m |= r.needs; });
  return m;
}

/* What the board uses a pin for, for the "why can I not have this" text. */
function boardFnNames(pin) {
  var names = [];
  Object.keys(pinCaps.fn).forEach(function(k) {
    var bit = pinCaps.fn[k];
    if ((pin.f & bit) && (pinCaps.reserved_mask & bit)) names.push(k.replace(/_/g, ' '));
  });
  return names.join(', ');
}

function pinByGroup(g) {
  return pinCaps.pins.filter(function(p) { return p.g === g; });
}

/* ── Lua tab: the pin table ────────────────────────────────────── */

function renderLuaPins() {
  var mask = luaAnyMask();
  var rows = pinCaps.pins.filter(function(p) { return (p.f & mask) !== 0; });
  var html = '<tr><th>GPIO</th><th>On the board</th><th>Role</th><th>Name in Lua</th></tr>';

  rows.forEach(function(p) {
    var what = boardFnNames(p) || 'user pad';
    if (p.g !== 'none') what += ' (' + p.g + ')';
    html += '<tr><td class="lbl">GPIO' + p.p + '</td><td class="val">' + esc(what) + '</td>';
    if (p.held) {
      html += '<td colspan="2" class="warn-inline">held by the flight software' +
              ' — release it on the Config tab</td>';
    } else {
      html += '<td><select id="pr' + p.p + '" onchange="luaPinsCheck()">';
      rolesFor(p).forEach(function(r) {
        html += '<option value="' + esc(r.r) + '"' + (r.r === p.role ? ' selected' : '') +
                '>' + esc(roleLabel(r.r)) + '</option>';
      });
      html += '</select></td><td><input id="pn' + p.p + '" maxlength="8" size="9" value="' +
              esc(p.name) + '" oninput="luaPinsCheck()"></td>';
    }
    html += '</tr>';
  });

  document.getElementById('luaPins').innerHTML = html;
  document.getElementById('luaPinsHint').innerHTML =
    esc(pinCaps.board) + ': ' + rows.length + ' assignable pad' + (rows.length === 1 ? '' : 's') +
    '. A pad is only ever plain GPIO or a PIO state machine, never a peripheral function, so' +
    ' nothing here can reach the pressure sensor or the buzzer.';
  luaPinsCheck();
}

/* The firmware validates the whole assignment on POST and is authoritative.
 * This catches the two mistakes that are easy to make and annoying to make
 * twice, before the round trip. */
function luaPinsCheck() {
  if (!pinCaps) return;
  var msgs = [], halves = [], names = {}, dupes = [];

  pinCaps.pins.forEach(function(p) {
    var sel = document.getElementById('pr' + p.p);
    if (!sel) return;
    if (sel.value === 'bridge') halves.push(p);
    if (sel.value !== 'off') {
      var n = (document.getElementById('pn' + p.p).value || '').trim();
      if (n) {
        if (names[n]) dupes.push(n); else names[n] = true;
      }
    }
  });

  if (halves.length) {
    var ch = halves.filter(function(p) { return p.g === 'ch1' || p.g === 'ch2'; }).length;
    var cm = halves.filter(function(p) { return p.g === 'common'; }).length;
    if (ch !== 1 || cm !== 1) {
      msgs.push('<div class="warn">A half-bridge is one channel element plus the common. ' +
                'Selected: ' + ch + ' channel, ' + cm + ' common.</div>');
    } else {
      msgs.push('<div class="tips"><b>Half-bridge selected.</b> ' +
                esc(pinCaps.protection_note) + '</div>');
    }
  }
  if (dupes.length) {
    msgs.push('<div class="warn">Two pads share the name &ldquo;' + esc(dupes[0]) +
              '&rdquo;. A script resolves the first one and never reaches the second.</div>');
  }
  document.getElementById('luaPinsWarn').innerHTML = msgs.join('');
}

/* Posts only the roles and names. The release flags belong to the Config tab
 * and the firmware merges this over the live assignment, so each tab writes
 * what it owns and leaves the rest alone. */
function pinsSave() {
  var ini = '[pins]\r\n';
  pinCaps.pins.forEach(function(p) {
    var sel = document.getElementById('pr' + p.p);
    if (!sel) return;
    ini += 'p' + p.p + '_role=' + sel.value + '\r\n';
    ini += 'p' + p.p + '_name=' + (document.getElementById('pn' + p.p).value || '').trim() + '\r\n';
  });
  postPins(ini, 'luaPinsMsg', renderLuaPins);
}

function postPins(ini, msgId, after) {
  var msg = document.getElementById(msgId);
  msg.style.color = ''; msg.textContent = ' saving…';
  fetch('/api/pins', {method:'POST', headers:{'Content-Type':'text/plain'}, body: ini})
    .then(function(r) { return r.json().catch(function(){ return {error:'HTTP ' + r.status}; }); })
    .then(function(d) {
      if (d.error) {
        msg.style.color = 'red';
        msg.textContent = ' ✗ ' + d.error + (d.pin !== undefined ? ' (GPIO' + d.pin + ')' : '');
        return;
      }
      msg.style.color = 'green';
      msg.textContent = d.reboot_required ? ' ✓ saved — reboot to apply' : ' ✓ saved';
      return pinsFetch().then(function() { if (after) after(); });
    })
    .catch(function(e) { msg.style.color = 'red'; msg.textContent = ' ✗ ' + e.message; });
}

/* ── Config tab: releasing pyro pins ───────────────────────────── */

function renderRelease() {
  var hint = document.getElementById('relHint');
  var ch1 = pinByGroup('ch1'), ch2 = pinByGroup('ch2'), common = pinByGroup('common');
  if (!ch1.length && !ch2.length) {
    hint.textContent = esc(pinCaps.board) + ' declares no releasable pyro pins.';
    return;
  }

  var side = pinCaps.topology === 'high_switched' ? 'high side' : 'low side';
  var other = pinCaps.topology === 'high_switched' ? 'low side' : 'high side';
  hint.innerHTML = esc(pinCaps.board) + ' switches a per-channel ' + side +
    ' and shares one ' + other + '. A channel releases on its own; the shared element' +
    ' releases only once both are, because until then it is still half of the' +
    ' retained channel’s firing path.';

  document.getElementById('rel1').checked = !!pinCaps.pyro1_released;
  document.getElementById('rel2').checked = !!pinCaps.pyro2_released;
  document.getElementById('rel1pins').textContent = ch1.map(function(p){return 'GPIO'+p.p;}).join(', ');
  document.getElementById('rel2pins').textContent = ch2.map(function(p){return 'GPIO'+p.p;}).join(', ');
  document.getElementById('relCommon').textContent = common.length
    ? common.map(function(p){return 'GPIO'+p.p;}).join(', ') +
      (common[0].held ? ' — held' : ' — released')
    : 'none';
  document.getElementById('relTable').style.display = '';
  document.getElementById('relBtns').style.display = '';
  relChanged();
}

function relChanged() {
  if (!pinCaps) return;
  var r1 = document.getElementById('rel1').checked;
  var r2 = document.getElementById('rel2').checked;
  var msgs = [];

  if (r1 !== r2) {
    /* The firmware cannot prevent this one, so it has to be said plainly. */
    msgs.push('<div class="warn"><b>One channel released, one retained.</b> Firing the' +
      ' retained channel asserts the shared element for 500 ms, and for that window the' +
      ' released pad has a return path — whatever is wired to it will carry current.' +
      ' The released side is a plain digital pin and nothing in the firmware knows what' +
      ' you connected.</div>');
  }
  if ((r1 || r2) && !(r1 && r2)) {
    msgs.push('<div class="tips">The shared element stays with the flight software until' +
      ' both channels are released, so this gives Lua one pad.</div>');
  }
  if (r1 && r2 && pinCaps.bridge_possible) {
    msgs.push('<div class="tips">With both released you get three digital pads, or a' +
      ' half-bridge plus one digital pad. ' + esc(pinCaps.protection_note) + '</div>');
  }
  if ((!r1 && pinCaps.pyro1_released) || (!r2 && pinCaps.pyro2_released)) {
    msgs.push('<div class="tips">Taking a channel back clears any Lua role on its pads.</div>');
  }
  document.getElementById('relWarn').innerHTML = msgs.join('');
}

/* Posts the release flags, and clears the Lua role off any pad being taken
 * back -- a role left on a re-retained pad fails validation, and the whole
 * file is then rejected, which is a confusing way to learn you unticked a
 * box. */
function relSave() {
  var r1 = document.getElementById('rel1').checked;
  var r2 = document.getElementById('rel2').checked;
  var ini = '[pins]\r\npyro1_released=' + r1 + '\r\npyro2_released=' + r2 + '\r\n';

  var retaking = [];
  if (!r1) retaking = retaking.concat(pinByGroup('ch1'));
  if (!r2) retaking = retaking.concat(pinByGroup('ch2'));
  if (!r1 || !r2) retaking = retaking.concat(pinByGroup('common'));
  retaking.forEach(function(p) {
    ini += 'p' + p.p + '_role=off\r\np' + p.p + '_name=\r\n';
  });

  postPins(ini, 'relMsg', function() { renderRelease(); renderLuaPins(); });
}

var relReady = false;

function relInit() {
  if (relReady) return;
  relReady = true;
  pinsFetch().then(renderRelease).catch(function(e) {
    document.getElementById('relHint').textContent =
      'could not read the capability table: ' + e.message;
  });
}

var luaReady = false;
var luaConTimer = null;

function luaInit() {
  if (!luaReady) {
    luaReady = true;
    luaLoad();
  }
  if (!luaConTimer) luaConTimer = setInterval(luaConPoll, 1000);
}

function luaLoad() {
  fetch('/api/config').then(function(r){return r.text()}).then(function(t) {
    var kv = {};
    t.split('\n').forEach(function(line) {
      var i = line.indexOf('=');
      if (i > 0) kv[line.slice(0,i).trim()] = line.slice(i+1).trim();
    });
    document.getElementById('luEn').checked = (kv.lua_enabled === 'true');
    document.getElementById('luBaud').value = kv.lua_baud || '9600';
    document.getElementById('luPx').value   = kv.lua_pixels || '0';
  });
  /* Pin roles live in pins.ini, not here. The lua_p18..p21 keys this used to
     read are migration-only now: the firmware consults them once, when a
     board has no pins.ini yet. */
  pinsFetch().then(renderLuaPins).catch(function(e) {
    document.getElementById('luaPinsHint').textContent =
      'could not read the capability table: ' + e.message;
  });
  fetch('/api/lua/script').then(function(r){return r.ok?r.text():''}).then(function(t) {
    document.getElementById('luSrc').value = t;
  });
}

function luaCfgIni() {
  var ini = '[pyro]\r\nlua_enabled=' + (document.getElementById('luEn').checked ? 'true':'false') +
            '\r\nlua_baud='   + (parseInt(document.getElementById('luBaud').value) || 9600) +
            '\r\nlua_pixels=' + (parseInt(document.getElementById('luPx').value) || 0) + '\r\n';
  return ini;
}

function luaShowResult(d) {
  var box = document.getElementById('luChk');
  if (!d || !d.items) { box.innerHTML = ''; return; }
  var html = '<div class="' + (d.green ? 'ok' : 'warn') + '">' +
             (d.green ? '✓ ready for flight' : '✗ not ready') + '</div><ul>';
  d.items.forEach(function(it) {
    var tag = {0:'ok', 1:'syntax', 2:'missing', 3:'warning'}[it.kind] || '?';
    html += '<li><b>' + tag + ':</b> ' + it.detail.replace(/</g,'&lt;') + '</li>';
  });
  box.innerHTML = html + '</ul>';
}

function luaCheck() {
  fetch('/api/lua/check', {method:'POST', headers:{'Content-Type':'text/plain'},
                           body: document.getElementById('luSrc').value})
    .then(function(r){return r.json()}).then(luaShowResult)
    .catch(function(){ document.getElementById('luChk').textContent = 'check failed'; });
}

function luaSave() {
  var box = document.getElementById('luChk');
  box.textContent = 'saving…';
  /* Config first, so the check on the device runs against the resource set
     the operator just chose rather than the previous one. */
  fetch('/api/config', {method:'POST', headers:{'Content-Type':'text/plain'}, body: luaCfgIni()})
    .then(function() {
      return fetch('/api/lua/script', {method:'POST', headers:{'Content-Type':'text/plain'},
                                       body: document.getElementById('luSrc').value});
    })
    .then(function(r) {
      if (!r.ok) throw new Error('upload rejected');
      /* The upload replies 201 Created, not JSON. Ask for the verdict
         separately so it is computed against what is now stored. */
      return fetch('/api/lua/check', {method:'POST', headers:{'Content-Type':'text/plain'},
                                      body: document.getElementById('luSrc').value});
    })
    .then(function(r){ return r.json(); })
    .then(function(d) {
      luaShowResult(d);
      box.innerHTML += d.green
        ? '<div class="warn">Saved. Reboot to load it on core 1.</div>'
        : '<div class="warn">Saved, but it will not be started until this is green.</div>';
    })
    .catch(function(){ box.textContent = 'save failed'; });
}

function luaConPoll() {
  if (document.getElementById('tab-lua').style.display === 'none') return;
  fetch('/api/lua/console').then(function(r){return r.ok?r.json():null}).then(function(d) {
    if (!d) return;
    document.getElementById('luState').textContent = d.status || '—';
    document.getElementById('luHb').textContent = d.heartbeat;
    if (d.text) {
      var pre = document.getElementById('luCon');
      pre.textContent += d.text;
      if (pre.textContent.length > 8000) pre.textContent = pre.textContent.slice(-6000);
      if (document.getElementById('luFollow').checked) pre.scrollTop = pre.scrollHeight;
    }
  }).catch(function(){});
}

function luaConClear() { document.getElementById('luCon').textContent = ''; }

/* ── Init ──────────────────────────────────────────────────────── */
update();
