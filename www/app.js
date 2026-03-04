function update() {
  fetch('/api/status').then(r => r.json()).then(d => {
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
    document.getElementById('status').innerHTML = s;
  }).catch(() => {
    document.getElementById('status').innerHTML = 'Connection lost';
  });
  setTimeout(update, 500);
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

update();
