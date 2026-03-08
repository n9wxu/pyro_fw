/*
 * Service worker that intercepts /api/* calls and responds with mock data.
 * Simulates a Pyro MK1B flight computer in 3 modes.
 */

const MODES = {
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

let currentMode = 'new';
let status = null;
let configIni = '';
let pendingConfig = null;

function reset(mode) {
  currentMode = mode;
  status = JSON.parse(JSON.stringify(MODES[mode]));
  configIni = buildIni(status);
  pendingConfig = null;
}

function buildIni(s) {
  return `[pyro]\r\nid=${s.rocket_id}\r\nname=${s.rocket_name}\r\n` +
    `pyro1_mode=${s.pyro1_mode}\r\npyro1_value=${s.pyro1_value}\r\n` +
    `pyro2_mode=${s.pyro2_mode}\r\npyro2_value=${s.pyro2_value}\r\n` +
    `units=${['cm','m','ft'][s.units]}\r\n`;
}

function flightCSV() {
  if (currentMode !== 'flown') return 'time_ms,pressure_pa,altitude_cm,state,event\n';
  const g = 101325;
  const pts = [
    [0,0,0,'LAUNCH'],[1000,30000,1,''],[2000,90000,1,''],[3000,170000,1,''],
    [4000,230000,1,''],[5000,270000,1,''],[6000,295000,1,''],[7000,303000,1,''],
    [7500,304500,1,'ARMED'],[8000,304800,1,''],[8100,304800,2,'APOGEE'],
    [8100,304800,2,'PYRO1'],[10000,280000,2,''],[12000,250000,2,''],
    [14000,220000,2,''],[16000,190000,2,''],[18000,160000,2,''],
    [20000,130000,2,''],[22000,100000,2,''],[24000,70000,2,''],
    [26000,40000,2,''],[28000,15240,2,'PYRO2'],[29000,8000,2,''],
    [30000,3000,2,''],[31000,500,2,''],[32000,15,2,''],[32400,0,3,'LANDING']
  ];
  let csv = 'time_ms,pressure_pa,altitude_cm,state,event\n';
  for (const [t,alt,st,evt] of pts) csv += `${t},${Math.round(g-alt*10/83)},${alt},${st},${evt}\n`;
  return csv;
}

reset('new');

self.addEventListener('install', e => self.skipWaiting());
self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));

self.addEventListener('message', e => {
  if (e.data && e.data.type === 'setMode') {
    reset(e.data.mode);
    e.source.postMessage({ type: 'modeChanged', mode: currentMode });
  }
});

self.addEventListener('fetch', e => {
  const url = new URL(e.request.url);
  const path = url.pathname.replace(/.*\/api/, '/api');

  if (path === '/api/status' && e.request.method === 'GET') {
    status.uptime += 1000;
    e.respondWith(new Response(JSON.stringify(status), {
      headers: { 'Content-Type': 'application/json' }
    }));
    return;
  }

  if (path === '/api/config' && e.request.method === 'GET') {
    e.respondWith(new Response(configIni, {
      headers: { 'Content-Type': 'text/plain' }
    }));
    return;
  }

  if (path === '/api/config' && e.request.method === 'POST') {
    e.respondWith((async () => {
      const body = await e.request.text();
      configIni = body;
      pendingConfig = body;
      return new Response('OK', { status: 201 });
    })());
    return;
  }

  if (path === '/api/reboot' && e.request.method === 'POST') {
    if (pendingConfig) {
      for (const line of pendingConfig.split(/\r?\n/)) {
        const eq = line.indexOf('=');
        if (eq < 0) continue;
        const k = line.substring(0, eq), v = line.substring(eq + 1);
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
    e.respondWith(new Response('Rebooting'));
    return;
  }

  if (path === '/api/flight.csv' && e.request.method === 'GET') {
    e.respondWith(new Response(flightCSV(), {
      headers: { 'Content-Type': 'text/csv', 'Content-Disposition': 'attachment; filename="flight.csv"' }
    }));
    return;
  }

  if (path === '/api/ota' && e.request.method === 'POST') {
    e.respondWith(new Response('OK (simulated)'));
    return;
  }
});
