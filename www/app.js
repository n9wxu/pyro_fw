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
      msg.style.color = r.ok ? 'green' : 'red';
      msg.textContent = r.ok ? ' Done, rebooting...' : ' Error!';
    }).catch(() => {
      msg.style.color = 'green';
      msg.textContent = ' Rebooting...';
    });
  });
}

var GITHUB_REPO = 'n9wxu/pyro_fw';
var ASSET_NAME = 'pyro_fw_c_fota_image.bin';
var currentVersion = null;

function checkUpdate() {
  var msg = document.getElementById('updmsg');
  msg.style.color = 'orange';
  msg.textContent = ' Checking...';
  if (!currentVersion) {
    msg.textContent = ' Waiting for device status...';
    return;
  }
  fetch('https://api.github.com/repos/' + GITHUB_REPO + '/releases/latest')
    .then(r => {
      if (!r.ok) throw new Error('No releases found (HTTP ' + r.status + ')');
      return r.json();
    })
    .then(rel => {
      if (!rel.tag_name) throw new Error('No releases published yet');
      var ver = rel.tag_name.replace(/^v/, '');
      if (ver === currentVersion) {
        msg.style.color = 'green';
        msg.textContent = ' Up to date (v' + ver + ')';
      } else {
        msg.style.color = 'blue';
        msg.innerHTML = ' Update available: v' + currentVersion + ' → v' + ver +
          ' <button onclick="doUpdate(\'' + rel.tag_name + '\')">⬆ Install</button>';
      }
    })
    .catch(e => {
      msg.style.color = 'red';
      msg.textContent = ' Cannot reach GitHub: ' + e.message;
    });
}

function doUpdate(tag) {
  var msg = document.getElementById('updmsg');
  msg.style.color = 'orange';
  msg.textContent = ' Downloading from GitHub...';
  fetch('https://api.github.com/repos/' + GITHUB_REPO + '/releases/tags/' + tag)
    .then(r => r.json())
    .then(rel => {
      var asset = rel.assets.find(a => a.name === ASSET_NAME);
      if (!asset) throw new Error(ASSET_NAME + ' not found in release');
      return fetch(asset.browser_download_url);
    })
    .then(r => {
      if (!r.ok) throw new Error('Download failed: ' + r.status);
      msg.textContent = ' Downloading... (' + (r.headers.get('content-length') || '?') + ' bytes)';
      return r.arrayBuffer();
    })
    .then(buf => {
      msg.textContent = ' Flashing ' + buf.byteLength + ' bytes...';
      return fetch('/api/ota', {
        method: 'POST',
        body: new Uint8Array(buf)
      });
    })
    .then(r => {
      msg.style.color = 'green';
      msg.textContent = ' Done, rebooting...';
    })
    .catch(e => {
      if (e.message.includes('Failed to fetch') || e.message.includes('NetworkError')) {
        msg.style.color = 'green';
        msg.textContent = ' Rebooting...';
      } else {
        msg.style.color = 'red';
        msg.textContent = ' Error: ' + e.message;
      }
    });
}

update();
