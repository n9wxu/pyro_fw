/*
 * The Lua and Config tabs, rendered against real board responses.
 *
 * This is the test the drift needed. app.js used to hardcode MK1C's four J3
 * pads and its own role list, so on MK1A and MK1B it offered four pads that
 * are not there and roles no pin on those boards can take -- and nothing
 * caught it, because no test ever rendered the page.
 *
 * The fixtures are captured from /api/pins/caps on each board, so a change to
 * the firmware's capability table, role vocabulary or response shape shows up
 * here as a failure rather than as a tab that silently offers the wrong pins.
 * Refresh one with:
 *
 *     curl -s http://<board>/api/pins/caps | python3 -m json.tool \
 *         > test/fixtures/pins_caps_<board>.json
 *
 * SPDX-License-Identifier: MIT
 */
'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
let failures = 0, checks = 0;

function ok(cond, what) {
  checks++;
  if (!cond) { failures++; console.log('  FAIL: ' + what); }
}
function eq(a, b, what) {
  ok(a === b, what + '  (got ' + JSON.stringify(a) + ', want ' + JSON.stringify(b) + ')');
}

/* ── A DOM stub, and the module under test ─────────────────────────
 *
 * app.js is a plain script against a live page, so the section under test is
 * evaluated in isolation with the two posting calls replaced by a return of
 * the INI they would have sent. That keeps the test on the rendering and the
 * rules, which is where the drift was. */
function load(capsFile) {
  const src = fs.readFileSync(path.join(ROOT, 'www/app.js'), 'utf8');
  const start = src.indexOf('/* ── Pin capabilities');
  const end = src.indexOf('function luaLoad()');
  if (start < 0 || end < 0) throw new Error('app.js no longer has a pin-capability section');

  const body = src.slice(start, end)
    .replace("postPins(ini, 'luaPinsMsg', renderLuaPins);", 'return ini;')
    .replace("postPins(ini, 'relMsg', function() { renderRelease(); renderLuaPins(); });", 'return ini;');

  const els = {};
  const mk = (id, init) => (els[id] = Object.assign(
    {id, innerHTML: '', textContent: '', value: '', checked: false, style: {}}, init || {}));
  ['luaPins','luaPinsHint','luaPinsWarn','relHint','relTable','relBtns','relWarn',
   'rel1','rel2','rel1pins','rel2pins','relCommon'].forEach(i => mk(i));

  const sandbox = {
    document: {getElementById: id => els[id] || null, querySelectorAll: () => ({forEach() {}})},
    fetch: () => Promise.reject(new Error('the test does not reach the network')),
    setInterval: () => 0,
  };
  const make = new Function('document', 'fetch', 'setInterval',
    '"use strict";' + body +
    '\nreturn {renderLuaPins, renderRelease, relChanged, luaPinsCheck, pinsSave, relSave,' +
    ' rolesFor, luaAnyMask, setCaps: c => { pinCaps = c; }};');
  const M = make(sandbox.document, sandbox.fetch, sandbox.setInterval);

  const caps = JSON.parse(fs.readFileSync(path.join(ROOT, 'test/fixtures', capsFile), 'utf8'));
  M.setCaps(caps);

  /* Materialise the controls the render emits, the way a browser would. */
  M.materialise = () => {
    const html = els.luaPins.innerHTML;
    for (const m of html.matchAll(/<select id="(pr\d+)"[^>]*>([\s\S]*?)<\/select>/g)) {
      const opts = [...m[2].matchAll(/<option value="([^"]*)"( selected)?>/g)];
      mk(m[1], {value: (opts.find(o => o[2]) || opts[0])[1], choices: opts.map(o => o[1])});
    }
    for (const m of html.matchAll(/<input id="(pn\d+)"[^>]*value="([^"]*)"/g)) mk(m[1], {value: m[2]});
  };
  return {M, els, caps};
}

const text = h => h.replace(/<[^>]+>/g, ' ').replace(/\s+/g, ' ').trim();

/* ── Every board renders its own pads, and only its own ──────────── */

for (const board of ['mk1a', 'mk1b', 'mk1c']) {
  console.log('[' + board + ']');
  const {M, els, caps} = load('pins_caps_' + board + '.json');
  M.renderLuaPins();
  M.materialise();

  const shown = [...els.luaPins.innerHTML.matchAll(/<td class="lbl">GPIO(\d+)</g)].map(m => +m[1]);
  const mask = M.luaAnyMask();
  const expect = caps.pins.filter(p => (p.f & mask) !== 0).map(p => p.p);
  eq(JSON.stringify(shown), JSON.stringify(expect), 'renders exactly the pads with a Lua capability');
  ok(shown.length > 0, 'renders at least one pad');

  /* The rule the firmware enforces, checked against what the menu offers. */
  caps.pins.forEach(p => {
    const sel = els['pr' + p.p];
    if (!sel) { ok(p.held || (p.f & mask) === 0, 'GPIO' + p.p + ' has no menu only because it is held or incapable'); return; }
    ok(!p.held, 'GPIO' + p.p + ' offers a menu only when it is not held');
    sel.choices.forEach(r => {
      const role = caps.roles.find(x => x.r === r);
      ok(role !== undefined, 'offered role "' + r + '" is one the firmware knows');
      ok(role.needs === 0 || (p.f & role.needs) !== 0,
         'GPIO' + p.p + ' is not offered "' + r + '" without the capability for it');
      ok(r !== 'bridge' || caps.bridge_possible,
         'GPIO' + p.p + ' is not offered "bridge" on a board that has no pair for it');
    });
    eq(sel.value, p.role, 'GPIO' + p.p + ' menu opens on the assigned role');
  });

  /* What the tab posts must round-trip the device's own assignment. */
  const ini = M.pinsSave();
  caps.pins.filter(p => !p.held && (p.f & mask)).forEach(p => {
    ok(ini.includes('p' + p.p + '_role=' + p.role), 'posts GPIO' + p.p + ' role unchanged');
    ok(ini.includes('p' + p.p + '_name=' + p.name), 'posts GPIO' + p.p + ' name unchanged');
  });
  ok(!/p\d+_role/.test(ini.split('\r\n').filter(l => {
    const m = l.match(/^p(\d+)_role/);
    return m && caps.pins.find(p => p.p === +m[1] && p.held);
  }).join('')), 'never posts a role for a held pad');

  /* The release warnings come from the board, not from a fixed string. */
  M.renderRelease();
  const hasPyro = caps.pins.some(p => p.g === 'ch1' || p.g === 'ch2');
  if (hasPyro) {
    ok(els.relHint.innerHTML.includes(caps.topology === 'high_switched' ? 'high side' : 'low side'),
       'release hint states this board\'s topology');
    els.rel1.checked = true; els.rel2.checked = false;
    M.relChanged();
    ok(text(els.relWarn.innerHTML).includes('One channel released, one retained'),
       'warns when one channel is released and one retained');
    els.rel2.checked = true;
    M.relChanged();
    eq(text(els.relWarn.innerHTML).includes(caps.protection_note), caps.bridge_possible,
       'shows the board\'s own protection note exactly when a bridge is possible');
  }
  console.log('  ' + checks + ' checks so far');
}

/* ── The two mistakes worth catching before the round trip ───────── */

{
  console.log('[bridge pairing and duplicate names]');
  const {M, els} = load('pins_caps_mk1b.json');
  M.renderLuaPins(); M.materialise();

  els.pr21.value = 'bridge'; els.pr15.value = 'off';
  M.luaPinsCheck();
  ok(text(els.luaPinsWarn.innerHTML).includes('one channel element plus the common'),
     'a lone bridge half is flagged');

  els.pr15.value = 'bridge';
  M.luaPinsCheck();
  ok(text(els.luaPinsWarn.innerHTML).includes('Half-bridge selected'), 'a complete pair is accepted');

  els.pr8.value = 'out'; els.pn8.value = 'led';
  els.pr22.value = 'in'; els.pn22.value = 'led';
  M.luaPinsCheck();
  ok(text(els.luaPinsWarn.innerHTML).includes('share the name'), 'a duplicate Lua name is flagged');

  /* Taking a channel back has to clear its roles, or the whole file is
     rejected and the operator is told about a pin they did not touch. */
  els.rel1.checked = true; els.rel2.checked = false;
  const ini = M.relSave();
  ok(ini.includes('p22_role=off'), 'un-releasing a channel clears its own pad');
  ok(ini.includes('p15_role=off'), 'un-releasing one channel clears the common too');
  ok(!ini.includes('p21_role=off'), 'leaves the still-released channel alone');
}

console.log('\n' + checks + ' checks, ' + failures + ' failures');
process.exit(failures ? 1 : 0);
