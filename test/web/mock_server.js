/*
 * Mock Pyro MK1B server for web UI testing.
 *
 * Modes:
 *   new        — Fresh device, default config, PAD_IDLE
 *   configured — Non-default config, PAD_IDLE
 *   flown      — Post 10,000ft flight, LANDED, pyros fired, flight data
 *
 * Usage: node mock_server.js [new|configured|flown] [port]
 */
const http = require('http');
const fs = require('fs');
const path = require('path');

const MODE = process.argv[2] || 'new';
const PORT = parseInt(process.argv[3]) || 3000;
const WWW = path.join(__dirname, '..', '..', 'www');

/* ── State per mode ───────────────────────────────────────────────── */

const DEFAULTS = {
  new: {
    state: 'PAD_IDLE', alt_cm: 0, max_alt_cm: 0, vspeed_cms: 0,
    pressure_pa: 101325, pyro1_cont: true, pyro2_cont: true,
    pyro1_adc: 48, pyro2_adc: 52, pyro1_fired: false, pyro2_fired: false,
    armed: false, flight_ms: 0, uptime: 5000, fw_version: '1.3.0',
    pyro1_mode: 'delay', pyro1_value: 0, pyro2_mode: 'agl', pyro2_value: 300,
    units: 1, rocket_id: 'PYRO001', rocket_name: 'My Rocke'
  },
  configured: {
    state: 'PAD_IDLE', alt_cm: 0, max_alt_cm: 0, vspeed_cms: 0,
    pressure_pa: 101325, pyro1_cont: true, pyro2_cont: true,
    pyro1_adc: 45, pyro2_adc: 50, pyro1_fired: false, pyro2_fired: false,
    armed: false, flight_ms: 0, uptime: 12000, fw_version: '1.3.0',
    pyro1_mode: 'delay', pyro1_value: 2, pyro2_mode: 'agl', pyro2_value: 500,
    units: 2, rocket_id: 'RACE01', rocket_name: 'Screamer'
  },
  flown: {
    state: 'LANDED', alt_cm: 15, max_alt_cm: 304800, vspeed_cms: 0,
    pressure_pa: 101320, pyro1_cont: false, pyro2_cont: false,
    pyro1_adc: 4, pyro2_adc: 6, pyro1_fired: true, pyro2_fired: true,
    armed: false, flight_ms: 32400, uptime: 45000, fw_version: '1.3.0',
    pyro1_mode: 'delay', pyro1_value: 0, pyro2_mode: 'agl', pyro2_value: 500,
    units: 2, rocket_id: 'RACE01', rocket_name: 'Screamer'
  }
};

let status = JSON.parse(JSON.stringify(DEFAULTS[MODE]));
let configIni = buildIni(status);
let pendingReboot = false;
let pendingConfig = null;

function buildIni(s) {
  return `[pyro]\r\nid=${s.rocket_id}\r\nname=${s.rocket_name}\r\n` +
    `pyro1_mode=${s.pyro1_mode}\r\npyro1_value=${s.pyro1_value}\r\n` +
    `pyro2_mode=${s.pyro2_mode}\r\npyro2_value=${s.pyro2_value}\r\n` +
    `units=${['cm','m','ft'][s.units]}\r\n`;
}

/* ── Flight CSV for flown mode ────────────────────────────────────── */
/* 10,000ft flight: boost 0-3s, coast 3-8s, apogee ~8s at 304800cm,
   drogue descent 8-28s, main at 500ft(15240cm) ~28s, landing ~32s */

function generateFlightCSV() {
  let lines = ['time_ms,pressure_pa,altitude_cm,state,event'];
  const g = 101325;
  const pts = [
    // time_ms, alt_cm, state, event
    [0, 0, 0, 'LAUNCH'],
    [1000, 30000, 1, ''],
    [2000, 90000, 1, ''],
    [3000, 170000, 1, ''],
    [4000, 230000, 1, ''],
    [5000, 270000, 1, ''],
    [6000, 295000, 1, ''],
    [7000, 303000, 1, ''],
    [7500, 304500, 1, 'ARMED'],
    [8000, 304800, 1, ''],
    [8100, 304800, 2, 'APOGEE'],
    [8100, 304800, 2, 'PYRO1'],
    [10000, 280000, 2, ''],
    [12000, 250000, 2, ''],
    [14000, 220000, 2, ''],
    [16000, 190000, 2, ''],
    [18000, 160000, 2, ''],
    [20000, 130000, 2, ''],
    [22000, 100000, 2, ''],
    [24000, 70000, 2, ''],
    [26000, 40000, 2, ''],
    [28000, 15240, 2, 'PYRO2'],
    [29000, 8000, 2, ''],
    [30000, 3000, 2, ''],
    [31000, 500, 2, ''],
    [32000, 15, 2, ''],
    [32400, 0, 3, 'LANDING'],
  ];
  for (const [t, alt, st, evt] of pts) {
    const pa = Math.round(g - alt * 10 / 83);
    lines.push(`${t},${pa},${alt},${st},${evt}`);
  }
  return lines.join('\n') + '\n';
}

/* ── HTTP Server ──────────────────────────────────────────────────── */

let pinsIni = '[pins]\r\npyro1_released=true\r\npyro2_released=true\r\n';

const MIME = {'.html':'text/html','.js':'application/javascript','.css':'text/css'};

const server = http.createServer((req, res) => {
  const cors = {'Access-Control-Allow-Origin':'*','Access-Control-Allow-Methods':'GET,POST,OPTIONS','Access-Control-Allow-Headers':'Content-Type'};

  if (req.method === 'OPTIONS') { res.writeHead(204, cors); res.end(); return; }

  /* API routes */
  if (req.url === '/api/status' && req.method === 'GET') {
    if (status._rebooting && Date.now() < status._rebooting) {
      res.writeHead(503, cors); res.end('Rebooting'); return;
    }
    status.uptime += 1000;
    res.writeHead(200, {...cors, 'Content-Type':'application/json'});
    res.end(JSON.stringify(status));
    return;
  }

  if (req.url === '/api/config' && req.method === 'GET') {
    res.writeHead(200, {...cors, 'Content-Type':'text/plain'});
    res.end(configIni);
    return;
  }

  if (req.url === '/api/config' && req.method === 'POST') {
    let body = '';
    req.on('data', c => body += c);
    req.on('end', () => {
      configIni = body;
      pendingConfig = body;
      res.writeHead(200, {...cors, 'Content-Type': 'application/json'});
      res.end(JSON.stringify({applied: false}));
    });
    return;
  }

  /* ── Pin assignment ────────────────────────────────────────────
   *
   * Added because the Config and Lua tabs now render from the firmware's
   * capability table. Without these the release UI never appears, so nothing
   * exercised it -- which is the same drift the endpoint exists to prevent.
   * Shaped like MK1B: one user pad, two pyro channels and a common, all
   * released. */
  if (req.url === '/api/pins/caps' && req.method === 'GET') {
    const FN = {pyro_fire:1, pyro_common:2, pyro_sense:4, buzzer:8, uart_tx:16, uart_rx:32,
                i2c_sda:64, i2c_scl:128, led:256, digital:65536, pwm:131072, serial:262144,
                pixel:524288, bridge:1048576, analog:2097152};
    res.writeHead(200, {...cors, 'Content-Type':'application/json'});
    res.end(JSON.stringify({
      board: 'Pyro MK1B', topology: 'high_switched', bridge_possible: true,
      protection_note: "This board's common path is a self-resetting PTC and the high-side " +
                       'switch current-limits with its fault line wired back.',
      pyro1_released: true, pyro2_released: true, reserved_mask: 511, fn: FN,
      roles: [{r:'off',needs:0},{r:'out',needs:FN.digital},{r:'pwm',needs:FN.pwm},
              {r:'in',needs:FN.digital},{r:'tx',needs:FN.serial},{r:'rx',needs:FN.serial},
              {r:'pixel',needs:FN.pixel},{r:'bridge',needs:FN.bridge}],
      pins: [
        {p:0,  f:FN.uart_tx, g:'none',   role:'off',    name:'',      held:true},
        {p:8,  f:FN.digital|FN.pwm|FN.serial|FN.pixel, g:'none', role:'out', name:'led', held:false},
        {p:15, f:FN.pyro_common|FN.digital|FN.bridge,  g:'common', role:'bridge', name:'motor_lo', held:false},
        {p:16, f:FN.buzzer,  g:'none',   role:'off',    name:'',      held:true},
        {p:21, f:FN.pyro_fire|FN.digital|FN.pwm|FN.bridge, g:'ch1', role:'bridge', name:'motor', held:false},
        {p:22, f:FN.pyro_fire|FN.digital|FN.pwm|FN.bridge, g:'ch2', role:'in', name:'probe', held:false},
        {p:26, f:FN.pyro_sense|FN.analog, g:'none', role:'off', name:'', held:true}
      ]
    }));
    return;
  }

  if (req.url === '/api/pins' && req.method === 'GET') {
    res.writeHead(200, {...cors, 'Content-Type':'text/plain'});
    res.end(pinsIni);
    return;
  }

  if (req.url === '/api/pins' && req.method === 'POST') {
    let body = '';
    req.on('data', c => body += c);
    req.on('end', () => {
      pinsIni = body;
      res.writeHead(200, {...cors, 'Content-Type':'application/json'});
      res.end(JSON.stringify({status:'ok', reboot_required:true}));
    });
    return;
  }

  if (req.url === '/api/reboot' && req.method === 'POST') {
    res.writeHead(200, cors);
    res.end('Rebooting');
    /* Simulate reboot: reject requests for 1.5s, then apply config */
    const rebootUntil = Date.now() + 1500;
    const origStatus = status.state;
    status._rebooting = rebootUntil;
    setTimeout(() => {
      if (pendingConfig) {
        for (const line of pendingConfig.split(/\r?\n/)) {
          const [k, v] = line.split('=');
          if (!v) continue;
          if (k === 'pyro1_mode') status.pyro1_mode = v;
          else if (k === 'pyro1_value') status.pyro1_value = parseInt(v);
          else if (k === 'pyro2_mode') status.pyro2_mode = v;
          else if (k === 'pyro2_value') status.pyro2_value = parseInt(v);
          else if (k === 'units') status.units = {cm:0,m:1,ft:2}[v] ?? 0;
          else if (k === 'id') status.rocket_id = v.substring(0, 8);
          else if (k === 'name') status.rocket_name = v.substring(0, 8);
        }
        pendingConfig = null;
      }
      status.uptime = 3000;
      delete status._rebooting;
    }, 1500);
    return;
  }

  if (req.url === '/api/flight.csv' && req.method === 'GET') {
    const csv = MODE === 'flown' ? generateFlightCSV() : 'time_ms,pressure_pa,altitude_cm,state,event\n';
    res.writeHead(200, {...cors, 'Content-Type':'text/csv', 'Content-Disposition':'attachment; filename="flight.csv"'});
    res.end(csv);
    return;
  }

  /* Static files from www/ */
  let filePath = req.url === '/' ? '/www/index.html' : req.url;
  const fsPath = path.join(WWW, '..', filePath);
  const ext = path.extname(fsPath);
  if (fs.existsSync(fsPath)) {
    res.writeHead(200, {'Content-Type': MIME[ext] || 'application/octet-stream'});
    fs.createReadStream(fsPath).pipe(res);
  } else {
    res.writeHead(404);
    res.end('Not found');
  }
});

server.listen(PORT, () => {
  console.log(`Mock Pyro server (${MODE}) on http://localhost:${PORT}`);
});

module.exports = { server, PORT, MODE };
