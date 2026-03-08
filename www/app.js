var missCount = 0;
function update() {
  fetch('/api/status').then(r => r.json()).then(d => {
    missCount = 0;
    var s = '<b>' + d.state + '</b>';
    s += ' Alt:' + (d.alt_cm/100).toFixed(1) + 'm';
    s += ' Max:' + (d.max_alt_cm/100).toFixed(1) + 'm';
    s += ' Spd:' + (d.vspeed_cms/100).toFixed(1) + 'm/s';
    s += ' Pa:' + d.pressure_pa;
    s += '<br>P1:' + (d.pyro1_fired ? 'FIRED' : d.pyro1_cont ? 'OK' : 'OPEN');
    s += '(' + d.pyro1_adc + ')';
    s += ' P2:' + (d.pyro2_fired ? 'FIRED' : d.pyro2_cont ? 'OK' : 'OPEN');
    s += '(' + d.pyro2_adc + ')';
    s += ' Up:' + (d.uptime/1000).toFixed(0) + 's';
    s += '<br><small>FW ' + d.fw_version + '</small>';
    currentVersion = d.fw_version;
    document.getElementById('status').innerHTML = s;
    loadConfigFromStatus(d);
  }).catch(() => {
    if (++missCount > 3)
      document.getElementById('status').innerHTML = 'Connection lost';
  });
  setTimeout(update, 1000);
}

function dlConfig() {
  fetch('/api/config').then(r => r.text()).then(d => {
    document.getElementById('cfg').value = d;
  });
}

function dlFlight() {
  window.location = '/api/flight.csv';
}

function uploadConfig() {
  fetch('/api/config', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: document.getElementById('cfg').value
  }).then(r => {
    document.getElementById('cfgmsg').textContent = r.ok ? ' Saved!' : ' Error!';
  });
}

function uploadWeb() {
  var file = document.getElementById('webfile').files[0];
  var path = document.getElementById('webpath').value;
  if (!file || !path) { alert('Select file and enter path'); return; }
  file.arrayBuffer().then(buf => {
    fetch(path, {
      method: 'POST',
      body: new Uint8Array(buf)
    }).then(r => {
      document.getElementById('webmsg').textContent = r.ok ? ' Uploaded!' : ' Error!';
    });
  });
}

function uploadFW() {
  var file = document.getElementById('fwfile').files[0];
  if (!file) { alert('Select a .bin file'); return; }
  var msg = document.getElementById('fwmsg');
  if (!confirm('Flash firmware? Device will reboot.')) return;
  msg.textContent = ' Uploading...';
  msg.style.color = 'orange';
  file.arrayBuffer().then(buf => {
    fetch('/api/ota', {
      method: 'POST',
      body: new Uint8Array(buf)
    }).then(r => {
      msg.style.color = 'orange';
      msg.textContent = ' Rebooting...';
      waitForReboot(msg);
    }).catch(() => {
      msg.style.color = 'orange';
      msg.textContent = ' Rebooting...';
      waitForReboot(msg);
    });
  });
}

function waitForReboot(msg) {
  var attempts = 0;
  var poll = setInterval(function() {
    attempts++;
    if (attempts > 30) {
      clearInterval(poll);
      msg.textContent = ' Device not responding. Refresh manually.';
      msg.style.color = 'red';
      return;
    }
    fetch('/api/status').then(r => r.json()).then(d => {
      clearInterval(poll);
      msg.style.color = 'green';
      msg.textContent = ' Updated to v' + d.fw_version;
      currentVersion = d.fw_version;
    }).catch(() => {});
  }, 2000);
}

var GITHUB_REPO = 'n9wxu/pyro_fw';

/* ── Config editor ──────────────────────────────────────────────── */
var MAX_ALT = {0: 800000, 1: 8000, 2: 26247};
var UNIT_LABELS = {0: 'cm', 1: 'm', 2: 'ft'};
var cfgLoaded = false;

function getUnits() { return parseInt(document.getElementById('cfgUnits').value); }
function getMaxAlt() { return MAX_ALT[getUnits()]; }
function getUnitLabel() { return UNIT_LABELS[getUnits()]; }

function cfgUnitsChanged() { cfgModeChanged(1); cfgModeChanged(2); cfgValChanged(1); cfgValChanged(2); }

function cfgModeChanged(ch) {
  var mode = document.getElementById('p'+ch+'mode').value;
  var unitSpan = document.getElementById('p'+ch+'unit');
  var valInput = document.getElementById('p'+ch+'val');
  if (mode === 'delay') { unitSpan.textContent = 'seconds'; valInput.max = 65535; }
  else if (mode === 'speed') { unitSpan.textContent = getUnitLabel() + '/s'; valInput.max = getMaxAlt(); }
  else { unitSpan.textContent = getUnitLabel(); valInput.max = getMaxAlt(); }
  cfgValChanged(ch);
  updateTips();
}

function cfgValChanged(ch) {
  var mode = document.getElementById('p'+ch+'mode').value;
  var val = parseInt(document.getElementById('p'+ch+'val').value) || 0;
  var warn = document.getElementById('p'+ch+'warn');
  if (mode !== 'delay' && val > getMaxAlt())
    warn.textContent = '⚠ Exceeds ' + getMaxAlt() + ' ' + getUnitLabel() + ' sensor limit — will be clamped';
  else warn.textContent = '';
}

function updateTips() {
  var tips = document.getElementById('cfgTips');
  var p1 = document.getElementById('p1mode').value;
  var p2 = document.getElementById('p2mode').value;
  var msgs = [];
  if (p1 === 'delay' && document.getElementById('p1val').value === '0')
    msgs.push('💡 Pyro 1 delay=0 fires at apogee (typical for drogue)');
  if (p2 === 'agl') {
    var v = parseInt(document.getElementById('p2val').value) || 0;
    var u = getUnits();
    var low = u===2 ? 200 : u===1 ? 60 : 6000;
    var high = u===2 ? 1000 : u===1 ? 300 : 30000;
    if (v > 0 && v < low) msgs.push('⚠ Pyro 2 AGL very low — main may deploy close to ground');
    if (v > high) msgs.push('⚠ Pyro 2 AGL high — main deploys early, long descent');
  }
  if (p1 === p2 && p1 !== 'delay')
    msgs.push('💡 Both pyros use same mode — consider different modes for redundancy');
  tips.style.display = msgs.length ? 'block' : 'none';
  tips.innerHTML = msgs.join('<br>');
}

function loadConfigFromStatus(d) {
  if (cfgLoaded || !d.pyro1_mode) return;
  cfgLoaded = true;
  document.getElementById('cfgUnits').value = d.units || 0;
  document.getElementById('p1mode').value = d.pyro1_mode || 'delay';
  document.getElementById('p1val').value = d.pyro1_value || 0;
  document.getElementById('p2mode').value = d.pyro2_mode || 'agl';
  document.getElementById('p2val').value = d.pyro2_value || 0;
  cfgUnitsChanged();
}

function saveConfig() {
  var uname = ['cm','m','ft'][getUnits()];
  var ini = '[pyro]\r\npyro1_mode=' + document.getElementById('p1mode').value +
    '\r\npyro1_value=' + document.getElementById('p1val').value +
    '\r\npyro2_mode=' + document.getElementById('p2mode').value +
    '\r\npyro2_value=' + document.getElementById('p2val').value +
    '\r\nunits=' + uname + '\r\n';
  var msg = document.getElementById('cfgSaveMsg');
  fetch('/api/config', {method:'POST', headers:{'Content-Type':'text/plain'}, body:ini})
    .then(function(r) {
      msg.style.color = r.ok ? 'green' : 'red';
      msg.textContent = r.ok ? ' Saved! Click Reboot to apply.' : ' Error saving';
    });
}

function rebootDevice() {
  if (!confirm('Reboot device?')) return;
  var msg = document.getElementById('cfgSaveMsg');
  msg.style.color = 'orange';
  msg.textContent = ' Rebooting...';
  cfgLoaded = false;
  fetch('/api/reboot', {method:'POST'}).catch(function(){});
  waitForReboot(msg);
}
var ASSET_NAME = 'pyro_fw_c_fota_image.bin';
var currentVersion = null;

function checkUpdate() {
  var msg = document.getElementById('updmsg');
  var includeBeta = document.getElementById('betaCheck').checked;
  msg.style.color = 'orange';
  msg.textContent = ' Checking...';
  if (!currentVersion) {
    msg.textContent = ' Waiting for device status...';
    return;
  }
  var url = includeBeta
    ? 'https://api.github.com/repos/' + GITHUB_REPO + '/releases'
    : 'https://api.github.com/repos/' + GITHUB_REPO + '/releases/latest';
  fetch(url)
    .then(r => {
      if (!r.ok) throw new Error('No releases found (HTTP ' + r.status + ')');
      return r.json();
    })
    .then(data => {
      var rel = Array.isArray(data) ? data[0] : data;
      if (!rel || !rel.tag_name) throw new Error('No releases published yet');
      var ver = rel.tag_name.replace(/^v/, '');
      if (ver === currentVersion) {
        msg.style.color = 'green';
        msg.textContent = ' Up to date (v' + ver + ')';
      } else {
        var asset = rel.assets.find(a => a.name === ASSET_NAME);
        if (!asset) throw new Error(ASSET_NAME + ' not found in release');
        msg.style.color = 'blue';
        msg.innerHTML = ' v' + currentVersion + ' → v' + ver +
          ' <a href="' + asset.browser_download_url + '">⬇ Download</a>' +
          ' then use Upload Firmware below.' +
          '<br><small>Or run: <code>python3 support/update_from_release.py</code></small>';
      }
    })
    .catch(e => {
      msg.style.color = 'red';
      msg.textContent = ' ' + e.message;
    });
}

update();
