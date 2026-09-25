# Pyro MK1B Firmware - Current Status

_Last updated: 2026-09-23 — v2.1.466, dispatch handshake race closed_

## 🐞 Dispatch handshake race (fixed 2026-09-23)

`lua_core1_idle_wait()` mirrored `c1_go` into `c1_seen` **before** setting
`c1_busy`. Core0 calls core1 idle when `c1_go == c1_seen && c1_busy == 0`, so
between those two stores neither condition held and core0 would open the flash
window and erase into a core1 about to fetch from XIP — the deadlock in
`docs/core1_hazard.md`.

It lands as an intermittent watchdog reset reported as "died in stage 8",
because the slack loop is where HTTP-driven flash writes run. Seen twice while
POSTing during a Lua run; a 259 s soak did not reproduce it, which fits a
two-instruction window.

Setting `c1_busy` first, with a barrier, makes the two conditions overlap so
one always holds. Affects every board and every configuration, not just the
bridge.

## ✅ Beep personalities, defaulting to Eggtimer (2026-09-24)

A beep tells an operator what to DO. There are four things they can do standing
at a rocket, so there are four outcomes — and the diagnosis, which is finer
than that, is named on `/api/status` where it can be read instead of counted.

**The shipped personality follows Eggtimer Rocketry**, whose convention most
fliers already have in their ear ([Quantum
1.09G](https://eggtimerrocketry.com/wp-content/uploads/2024/02/Eggtimer-Quantum-1_09G.pdf),
[Quark
D3](https://eggtimerrocketry.com/wp-content/uploads/2021/04/Eggtimer-Quark-Manual-D3.pdf),
[Classic 1.48b](https://www.manualslib.com/manual/733694/Eggtimer-Rocketry-Eggtimer.html)):

| Outcome | Sound | Eggtimer source |
|---|---|---|
| OK to fly | rapid chirp, never counted | Quantum/Quark "ready to fly" |
| Check pyro 1 | 5 beeps | Quark: 5 = no Drogue continuity |
| Check pyro 2 | 4 beeps | Quark: 4 = no Main continuity |
| System failure | 2 beeps | Classic/TRS: 2 = sensor/hardware error |

The good case is a **texture, not a number**. Asking someone to count the case
meaning "everything is fine" is how it gets misheard in the wind — a vendor
thread shows a flier misreading Eggtimer's own counted codes badly enough that
an audio recording was needed to diagnose it.

**Three personality slots.** Slot 0 ships as the Eggtimer convention; the other
two are the operator's to name and shape. A personality is a complete
configuration — pattern per outcome (chirp / steady tone / beep count /
silent), cadence, and whether pyro faults are split per channel — because
those choices only make sense together.

**`BUZ-02` is gone.** It made the board say its state twice and fall silent,
which is indistinguishable from a board whose battery died a second later. The
default now re-announces every 5 s until launch, and the cadence is
configurable.

Refused, with the slot and outcome named: a beep count outside 1–9, two
outcomes that sound alike (including two chirps), an active slot that does not
exist, and a personality that says nothing at all.

Our altitude beep-out already matched Eggtimer Quantum exactly — long pause,
long beep, digit groups MSB-first, 10 beeps for zero — and is unchanged. That
is the closest thing to a real cross-brand convention; the *zero*
representation is not standard (RRC3 uses one long beep, Raven a short one).
Neither NAR nor Tripoli specifies anything about audible status.

Measured on hardware:

| Board | State | Says | Diagnosis |
|---|---|---|---|
| MK1B | pyros released to Lua | chirp — ok_to_fly | none |
| MK1C | pyros retained, no igniters | 5 beeps — check_pyro_1 | pyro1_open, pyro2_open |
| MK1A | pyros retained, no igniters | 5 beeps — check_pyro_1 | pyro1_open, pyro2_open |

### Also fixed: /api/status was truncating on MK1C

`char buf[1280]` holds headers *and* body, and MK1C carries the most fields —
the bias probes, pack voltage and wave state on top of everything shared. It
had begun cutting the body mid-word, and the handler's "send what fits" path
meant the browser got JSON that parses as nothing and no explanation. Now 2048,
and an overflow answers 500 saying so.

## ✅ config.ini fits again — and no longer migrates (2026-09-24)

`hal_config_load()` reads into `char[512]` and `http_server.c` refuses a merged
config that does not fit, but a fully populated config serialised to **525
bytes**: a board with its Lua pin names filled in could not save at all and
answered HTTP 500. Nothing tested the worst case, only the defaults.

The eight `lua_p18..p21_role/name` keys are **deleted**, not deprecated —
`pins.ini` superseded them and a clean wipe was chosen over migration. Gone
with them: `migrate_from_config()` in `pin_store.c`, the `role_of_legacy()`
helper, and `pin_store_load()`'s `config_t *` parameter. A board without
`pins.ini` now takes the board defaults — nothing released, every pyro pad
retained — and says so on `/api/status`.

| | Before | After | Budget |
|---|---|---|---|
| defaults | 445 | **313** | 512 |
| worst case | **525** | **341** | 512 |

`lua_app.c`'s `/api/lua/check` environment was also built from those keys. It
now reads the live assignment through `pin_store_lua_pins()` and
`pin_store_bridge()` — which fixes two things the old path got wrong anyway:
the keys were MK1C-shaped four-entry positional slots, so on MK1B they
described pads that do not exist, and they could not describe a released pyro
pad at all.

`test_config_worst_case_fits_the_budget` fills every string field, so the
budget is now checked rather than assumed.

## ✅ Power-up self-test: the sensor is tested first (item 10)

A board with a dead barometer used to beep `BEEP_ALL_GOOD` and sit on the pad.
`hal_pressure_init()` returns a sensor type and `flight_init()` discarded it;
`BOOT_CALIBRATE` then timed out after 10 s, sent a `!CAL TIMEOUT` line out the
telemetry UART only, and **returned `SEVT_CAL_DONE` anyway**, entering PAD_IDLE
as if healthy. `pp` stayed in `PP_CALIBRATING` forever, so `pp_read()` never
yielded a sample and the board could never have detected a launch.

Now: a new `BOOT_SENSOR` state runs **before** `BOOT_CONTINUITY`, so the
continuity verdict is never beeped on a board that cannot measure altitude.
A missing sensor, a calibration that produces no samples, or a filesystem that
will not mount each go to a terminal `FAULT` state which repeats the
corresponding code — `BEEP_SENSOR_FAIL` (4-1) or `BEEP_FS_FAIL` (4-2), both
defined since the beginning and emitted from nowhere until now.

`BOOT_SENSOR` and `FAULT` are **appended** to `flight_state_t`, not inserted:
state numbers reach the flight log, the CSV, the telemetry sentences and
`/api/status`, so renumbering PAD_IDLE would make every recorded flight read
wrong. The hardcoded `state < 7` bounds in `http_server.c` are gone.

Two related fixes:

- **Every fault is reported, not just the first.** The `else if` chain meant a
  board with both channels open reported only channel 1; the operator fixed it,
  heard the next code, and learned about the second fault on a second trip to
  the pad. `/api/status` now carries `faults[]`.
- **A released channel no longer false-alarms.** Its mocked continuity reads
  open by design, so the board was beeping "pyro 1 open" for a pad the operator
  deliberately gave to Lua.

`/api/status` gained `sensor_ok`, `fs_ok`, `fault_code` and `faults[]`.

Verified on MK1B by injecting a sensor failure: `state=FAULT`,
`fault_code=65` (beeps 4-1), terminal across 75 s, and still serving HTTP so
OTA can recover it. Healthy boot unchanged: `PAD_IDLE`, `sensor_ok=true`,
`faults=[]`.

## 🚧 Configurable pin assignment — in progress

Plan: pyro functions become movable or disableable, freed pins become
available to Lua, and the two web tabs assign them. Phase 0 shipped the
defects the feature would otherwise have been built on.

| Phase | Description | Status |
|-------|-------------|--------|
| 0 | `board_early_init()` caller, CFG-06 config merge, `/api/lua/check` escaping, two `snprintf` overflows | ✅ Done, HW verified |
| 0b | MK1C pyro tracking converted to MK1A's deadline-parked pattern | ✅ Done, HW verified |
| 1 | `boards/<board>/pin_caps.h` capability table + build-time assertions | ✅ Done, HW verified |
| 2 | `pins.ini` storage, `/api/pins`, power-group validation | ✅ Done, HW verified |
| 3 | Half-bridge Lua role (MK1A/MK1B), free-running sense getter | ✅ Done, HW verified |
| 3b | Released pads wired as GPIO; safing covers them | ✅ Done, HW verified |
| 3c | Generic Lua interface table; `prove_core0.py` follows vtables | ✅ Done |
| 4 | `GET /api/pins/caps`, Config and Lua tab pin UI | ✅ Done, HW verified |
| 4b | Released channels mocked at the HAL, logged and counted | ✅ Done, HW verified |
| 4c | One pad, one owner: the claim is what yields either vtable | ✅ Done, HW verified |
| 5 | Dead-time tuning against real FETs | 🔨 Next |

Release model: a pyro channel releases independently; the common low side
releases only once both channels are released. One channel released yields one
digital pin; both released yields three digital pins, or one half-bridge plus
one digital. MK1C's high side is the `ARM_TOGGLE` charge pump, so the bridge
role is refused there.

### Loop budget, measured 2026-09-22

`loop_max_us` is work only, slack excluded. All three boards pace to 10 ms.

| Board | per iteration | work max | stage 4 | overruns |
|-------|---------------|----------|---------|----------|
| MK1A | 10.01 ms | 1.7 ms | 0.3 ms | 0 |
| MK1B | 10.01 ms | 11.0 ms | 0.3 ms | 242 |
| MK1C | 10.04 ms | 2.7 ms | 1.2 ms | 0 |

### Pin capability tables (phase 1)

Each board declares one row per assignable pin in `pin_caps.h`: the functions
the hardware supports, and the pin's place in the pyro power group. A pyro pad
lists `FN_PYRO_*` together with what it may become once its channel is
released — holding both at once is an assignment-time rule (phase 2), not a
property of the hardware. A pin with no row is not assignable at all, which is
how MK1C's bias injectors stay unreachable.

| Board | Topology | Protection | Bridge |
|-------|----------|------------|--------|
| MK1A | high switched, common low | 8 A one-shot fuse | yes |
| MK1B | high switched, common low | 1.5 A PTC + AP2192 limit | yes |
| MK1C | low switched, common high | TPS259570 eFuse | no — the common is a charge pump |

`src/pin_model.h` holds the vocabulary and the `_Static_assert`s; a wrong row
fails the build. Both assertion families were negative-tested by deliberately
breaking a row.

### Assignment storage (phase 2)

`pins.ini`, served and accepted at `/api/pins`, merged over the live
assignment on POST for the same reason `/api/config` merges — a partial post
must not silently release a channel by omitting its key. Behind the PAD_IDLE
interlock and the flash window, like every other flash-writing POST.

A file that fails validation is **rejected whole** and the board falls back to
the migrated legacy assignment, with the reason on `/api/status` as
`pins_reason`. Half a release would leave the flight software and a script
each believing they own a pin.

With no `pins.ini`, the assignment is migrated from the `lua_p18..p21` keys
**positionally** against `LUA_PIN_LIST` — which is what those keys always
meant, since `lua_plat_configure()` indexed them by position. On MK1B
`lua_p18_role` configured GPIO8. Migrating by pin number instead would move
every existing board's Lua pins.

Verified on MK1C: release accepted and round-tripped; assigning the common
with one channel retained, a retained channel's pin, and a bridge on a board
that cannot make one were all refused with the file left unchanged.

### The half-bridge (phase 3)

PIO0 is the pyro block and PIO1 is Lua's. A released pyro pad is still a FET
gate behind a fuse, so whatever drives it runs on PIO0 under pyro rules
whoever is commanding it — which also leaves Lua's four state machines for
Lua's own roles. The assigned role picks the program; today that is one entry,
and general-purpose programs join it as roles need them.

Shoot-through is unencodable rather than merely avoided, which the assembled
program shows directly:

    8: set pins, 1  side 0    channel ON  => common OFF
   10: set pins, 0  side 1    common  ON  => channel OFF
    5: pull block   side 0    stall point, after both-off + dead band

The FIFO carries one bit choosing which single side turns on, so no value
encodes "both". Dead time is a count in Y, loaded from the first pushed word
and copied to X per transition, so it is tunable — phase 5 measures it against
the FETs each board fits. An empty FIFO stalls at instruction 5 with both
sides off, so a dead core1 stops the bridge rather than freezing it mid-drive.

Verified on MK1B: both channels released, bridge assigned across GPIO21 and
GPIO15, and a script toggling the midpoint every tick with the sense getter
reading it back. `pyro.status(n).released` distinguishes a released channel
from a pyro one, because continuity and armed stop meaning anything after a
release while the raw count keeps being sampled.

Not verified: power delivery into a real load. The bench has nothing wired to
the bridge, so this confirms the control path, not the drive.

### Released pads as GPIO (phase 3b)

The release model, confirmed on MK1B:

| Released | Lua gets | Verified |
|----------|----------|----------|
| one channel | its own high side as a GPIO | `outs: led,winch` |
| both | all three elements as GPIO | `outs: led,lo,hi1,hi2` |
| both | a half-bridge plus the third as GPIO | `outs: led,fan,motor` |

`lua_pin_cfg_t` now carries its own pin. It used to be indexed positionally
against `LUA_PIN_LIST`, which is what made the old `lua_p18..p21` keys
MK1C-shaped and left no way to configure a pad outside that list — so a
released pyro pad was validated, stored and then silently ignored.

`lua_plat_safe_outputs()` now walks every pad actually claimed rather than
`LUA_PIN_LIST`, so a released pad — including the bridge's two — is handed
back to SIO and driven low when core0 kills core1.

### The generic interface table (phase 3c)

`src/lua/lua_iface.h` replaces four parallel resource APIs — output, input,
serial, pixel — with one table of `{name, kind, vtable, ctx}`. Whoever
configures the hardware publishes into it; Lua resolves by name **and kind**.

That pairing is the safety property: `output.set()` on a pad published as an
input finds nothing, because no output vtable was ever installed for it. A
wrong combination is unreachable rather than refused, which is the same
argument as the half-bridge PIO program, where shoot-through is unencodable.
Dimmability lives in the vtable, not the instance, so "PWM on a pin that
cannot do it" is likewise a table that was never installed.

A board publishes a feature the shared platform knows nothing about by
implementing the weak `board_lua_publish()` hook, called at the end of
`lua_plat_configure()`. No board uses it yet; adding a board-unique interface
now costs a vtable rather than a new array, a new count, a new `find_` and a
new binding across four files.

**`prove_core0.py` had to grow with it.** The call graph is built from `bl`
instructions, so dispatch through a function pointer is invisible to it —
routing core1's hardware access through vtables would have severed the graph
and left the check passing while proving strictly less. It now reads the
`*_vt` symbols out of `.rodata`, resolves the stored addresses back to
function names, and folds those in as reachable from `core1_main`. Scoped to
that suffix on purpose: folding in *every* address-taken function pulls in
core0's HTTP route table and the littlefs callbacks, which reach flash
legitimately, and the check then fails on everything and means nothing. An
image that links `lua_iface_publish` and exposes no `*_vt` fails, so renaming
a vtable out of coverage is caught rather than silent.

Verified negatively: a `sleep_ms()` planted in a vtable entry — reachable from
core1 only through the pointer — is reported as `FAIL core1 can call
sleep_ms`. Before the change it passed.

### The web interface (phase 4)

`GET /api/pins/caps` serves the board's capability table, the live assignment,
**and the vocabulary to read both in**: `fn` names every capability bit and
`roles` names each role with the bit it requires. The browser had never
learned anything board-specific beyond the board name, which is why the Lua
tab rendered MK1C's four J3 pads on every board and offered roles no pin
there can take.

So `app.js` no longer keeps its own list of either. It filters each pin's menu
by the same rule `pin_assign_validate()` enforces — a capability bit tested
against the pin's mask — applied to the firmware's own table. What each board
now renders:

| Board | Pads offered | Bridge |
|---|---|---|
| MK1A | GPIO9, 10, 11 (pyro, held until released), 18, 19 | offered |
| MK1B | GPIO8, 15, 21, 22 | offered |
| MK1C | GPIO17, 18–22, 24 | not offered — no pad to pair with |

Nothing in the page needs changing when a board, a role or a capability bit is
added. A role this page has not been taught a friendly label for shows under
its firmware name rather than dropping out of the menu.

The shoot-through warning comes from `pin_caps_protection_note()`, so MK1A
says its one-shot fuse takes the pyros with it and MK1B says its PTC trips and
recovers. The one-channel-released warning is stated plainly, because the
firmware genuinely cannot prevent it: firing the retained channel asserts the
shared element for 500 ms and the released pad has a return path for that
window.

Each tab posts only what it owns — the Config tab the release flags, the Lua
tab the roles and names — and the firmware merges over the live assignment.
Un-releasing a channel also clears the Lua roles on its pads, since a role
left on a re-retained pad fails validation and gets the **whole file**
rejected, which is a confusing way to learn you unticked a box.

**`test/test_web_pins.js`** renders both tabs against `/api/pins/caps`
fixtures captured from all three boards: 316 checks that the pads rendered are
exactly the pads with a Lua capability, that no menu offers a role the pin
lacks the bit for, that the post round-trips the device's own assignment, and
that a lone bridge half and a duplicate name are caught. Verified negatively —
removing the capability filter produces 19 failures. It needs node, which is
not a build dependency, so `web_tests` skips with a message rather than
failing when node is absent.

### One pad, one owner (phase 4c)

The pyro table and the Lua interface table were independent. Nothing
structural stopped a pad from being in both — only `pin_assign_validate()`
and the order of two calls at boot, which is to say a check and a convention.
Get either wrong and two cores drive one FET gate.

`src/pad_claim.c` is now the single arbiter. `pin_store_claim_pads()` walks
the assignment **once** and gives every pad exactly one owner: there is one
array and one write per pad, so no code path can produce two answers. It
compares nothing.

After that, a claim is the only currency either table accepts:

- `pyro_release_claim()` installs a channel's real methods **only if it can
  claim that channel's pads** — its own element and the common, together.
  A channel whose pads went to Lua cannot take them, so it gets the mocked
  table. There is no branch there that could be written the other way.
- `lua_iface_publish()` takes the pads its resource drives and **fails unless
  Lua can claim all of them**, claiming them when it can.

So a pad cannot be in both tables: the second claimant's entry cannot be
created, because the claim it needed was already spent. Claims are all or
nothing, which is also what stops a half-claimed bridge — one FET gate owned
and one not.

`/api/status` now reports what the claim decided (`pyro1_real`, `pyro2_real`)
next to what the assignment asked for (`pyro1_released`, `pyro2_released`), so
a disagreement is visible rather than inferred from intent.

Verified negatively: removing the claim gate from `lua_iface_publish()`
produces 21 test failures; ignoring the claim in `pyro_release_claim()`
produces 8.

### Released channels are mocked, and say so (phase 4b)

The flight layer no longer reaches a released channel's hardware. Each
channel carries a table of operations — `fire`, `get`, `fault` — installed
once at boot from the assignment (`src/pyro_release.c`). A released channel
gets the mocked table, so firing it is not refused, it reaches a function that
touches nothing, because the real one was never installed for it. Same
argument as `lua_iface.h`.

**Nothing is silent.** Every mocked operation is reported three ways, each for
a different audience: a `MOCK` row in the flight log next to the event that
commanded it, a `!MOCK` line on the telemetry downlink at the time, and
`pyro_mocked` on `/api/status` afterwards. A flight log showing `PYRO1` with
nothing beside it would be a record of an ignition that did not happen.

A released channel reports **open**, not good. There is no igniter circuit the
flight software controls there any more, and reporting continuity would let it
arm and then "fire" a channel that cannot.

Two things a per-channel table cannot express, handled separately:

- **The shared stimulus.** `hal_pyro_sample()` and `hal_pyro_update()` drive
  the *common*, which is Lua's exactly when both channels are released, so
  both return early then. This is what took MK1B's loop overruns from about
  one per second to zero: its 10 ms stimulus is skipped entirely.
- **A per-channel pad under partial release.** MK1B's `pyro_sample()` drives
  both channel enables low as a precondition. On a released channel that pad
  is a Lua output, so core0 was stamping it low on every sample. It now asks
  `pin_store_owns()` first.

Measured across the three boards, which happened to be in three different
states:

| Board | Released | Claim kept | `pyro_mocked` | `pyro1_adc` / `pyro2_adc` | Overruns |
|---|---|---|---|---|---|
| MK1B | both | neither | climbing | 0 / 0 — both mocked | **0** (was ~1/s) |
| MK1C | pyro 1 | pyro 2 | climbing | 0 / 11 — one mocked, one real | 0 |
| MK1A | neither | both | **0** | 4081 / 4083 — both real | 0 |

`rel` and `real` are exact complements on every board: what the assignment
released is what the claim denied.

MK1A is the regression check: with nothing released, nothing changes.

MK1C was 36.2 ms work / 35.4 ms stage 4 / 3462 overruns before its bias tests
stopped calling `sleep_ms()` inside the flight loop. MK1B's remaining 11 ms is
`ms5607_read()`'s two `sleep_ms(MS5607_CONV_MS)` on the synchronous fallback
path — not yet converted.

### OTA transfer rate, measured 2026-09-22

MK1A transfers at roughly a third of the other boards and this follows the
board, not the cable or the host port (both swapped, timings unchanged).
Round-trip latency is identical, so it is the bulk flash-write path.

| Board | OTA (333 KB) | per 4 KB sector | 40 GETs |
|-------|--------------|-----------------|---------|
| MK1A | 81.4 s | 1.00 s | 12.6 ms each |
| MK1B | 27.3 s | 0.34 s | 12.8 ms each |
| MK1C | 34.1 s | 0.43 s | 20.1 ms each |


## 🔬 Hardware Serial Log Analysis (2026-04-04)

### Summary of Boot Cycles Captured

| FW Version | Boots | Behavior | False Launch? | Network? |
|------------|-------|----------|---------------|----------|
| 2.1.17 | ~8 | False launches on every boot | ❌ YES | N/A (no diag) |
| 2.1.24 | 2 | Stable PAD_IDLE, no false launch | ✅ FIXED | No diag |
| 2.1.26 | 2 | Stable PAD_IDLE, no false launch | ✅ FIXED | ❌ HTTP broken |

### FW 2.1.17 — False Launch Bug (now fixed)

Every boot cycle followed the same failure pattern:
1. BOOT_SETTLE → BOOT_CONT → BOOT_CAL → PAD_IDLE (normal, ~3.5s)
2. **PAD_IDLE → ASCENT in 0.5–6s** (false trigger)
3. ASCENT → DESCENT in **50ms** (impossible physics)
4. DESCENT → LANDED in ~1s
5. LANDED shows pressure slowly converging from millions of Pa to ~102,280 Pa (correct)
6. Device reboots after ~120–520s in LANDED (watchdog or power cycle)

Raw pressure during PAD_IDLE was wildly wrong: `2,689,215 Pa`, `-43,966 Pa`, `3,733,478 Pa` — 
values orders of magnitude outside the 30,000–110,000 Pa barometric range. The IIR filter's 
`dt = now - last_sample` overflowed when batch timestamps predated `last_sample`, producing 
`alpha` values that amplified 7 Pa of real noise into 1.8M Pa filter jumps.

**Buzzer impact during 2.1.17**: The rapid false-launch cycle meant:
- 10 startup chirps begin playing
- `buzzer_stop()` fires almost immediately (ASCENT triggered in <2s)
- `buzzer_set_altitude(800000cm)` starts altitude beep-out for 8000m (8,0,0,0 → 8 + 10 + 10 + 10 beeps)
- Extremely long garbled altitude pattern plays until watchdog reboot
- Cycle repeats: truncated chirps → cut off → garbled altitude beeps → reboot

### FW 2.1.24 — Pressure Processing Fix Verified ✅

Both boot cycles showed:
- Stable PAD_IDLE for 900+ seconds (entire captured duration)
- Pressure readings: 102,260–102,300 Pa (realistic, normal atmospheric drift)
- Altitude readings: 0–255 cm (sensor noise at ground level, normal)
- No false state transitions
- $PYRO telemetry: consistent 2Hz output with valid sensor data
- Pyro ADC values: 26–30 range on both channels (continuity readings)

**Buzzer behavior (2.1.24)**: Working correctly:
- 10 chirps (30ms on / 30ms off each) at startup
- Status code plays TWICE (BUZ-02 compliant), then stops
- Buzzer goes silent after ~3–4 seconds total
- Code depends on pyro continuity status (likely 2-1 or 3-1 if no ematches connected)

### FW 2.1.26 — Network Diagnostics Reveal HTTP Failure ❌

Both boot cycles showed stable flight software (no false launches), but HTTP is non-functional:

```
[10003] NET: tud=1 rx=67  drop=0 tx=8  txf=3   http=0
[50003] NET: tud=1 rx=90  drop=0 tx=9  txf=7   http=0
[70003] NET: tud=1 rx=95  drop=0 tx=9  txf=10  http=0
```

Second boot (worse):
```
[10005] NET: tud=1 rx=61  drop=0 tx=11 txf=2   http=0
[60005] NET: tud=1 rx=95  drop=0 tx=12 txf=120 http=0
[100005] NET: tud=1 rx=104 drop=0 tx=12 txf=270 http=0
[120005] NET: tud=1 rx=121 drop=0 tx=12 txf=308 http=0
```

Key observations:
- **tud=1**: USB device ready, host has enumerated the NCM device
- **rx growing**: Frames being received from host (ARP, DHCP, pings)
- **http=0**: Zero HTTP connections accepted — TCP listener not serving
- **txf growing rapidly**: USB TX failures (tud_network_xmit returning false)
- **tx stuck at 9–12**: Only a handful of frames ever transmitted successfully
- **drop=0**: No frames dropped at the receive stage

**Diagnosis**: The device receives network frames but cannot transmit responses. The
growing `txf` count indicates USB NCM TX buffer saturation or a protocol handshake issue.
The host keeps retrying (rx grows), but since responses fail, TCP connections never complete.
This affects ARP replies, DHCP, and TCP SYN-ACK — meaning HTTP can never be established.

**Next steps for network fix**:
1. Check if lwIP is properly calling `tud_network_xmit()` at the right time
2. Verify NCM TX is not being called before USB enumeration completes
3. Add timing guard: don't attempt TX until tud_ready() has been true for >1s
4. Check if the `__wfe()` sleep is preventing USB interrupt servicing

## ✅ Completed

### Event-Driven State Machine
- Transition table: 8 rows define every possible state change
- Detectors (per-tick work) → Events → Actions (side effects)
- 4 boot states + 4 flight states, no implementation leakage
- See IMPLEMENTATION.md for transition table

### Hardware Abstraction Layer
- hal.h: 15 functions covering all hardware interactionded
- Zero #ifdef in flight code (flight_states.c, telemetry_formatter.c, buzzer.c)
- Three implementations: hardware, test, simulation
- Same source compiles for Pico, host tests, and WASM

### Config System
- INI parser with 12 unit tests
- Web config editor (all fields: id, name, units, beep, pyro1, pyro2)
- Beep code 4-3 for out-of-range altitude settings
- 8,000m altitude clamp on all altitude-based settings

### Telemetry
- Format 0 (default): `$PYRO` NMEA sentences with XOR checksum, backwards-compatible
- Format 1: JSON newline-delimited objects (selectable via `telem_format=1` in config.ini)
- Event sentences: `$PYRO_APO`, `$PYRO_FIRE`, `$PYRO_LAND` on key flight events
- 10Hz ASCENT/DESCENT, 1Hz PAD_IDLE/LANDED
- `telemetry_formatter.c` — standalone protocol-independent module

### Buzzer (v2 Task 7) ✅ Done — Hardware Verified
- `async_task_t`-based pattern player — runs as a parallel state machine alongside pressure sampling
- Three states: BZ_IDLE → BZ_ENCODE_CODE/ALTITUDE → BZ_PLAYING
- Step table built at request time; `hal_tasks_tick()` fires each step at its deadline
- `buzzer_play_code(code, uint8_t repeat_count)` — v2 API with strict repeat semantics:
  `0`=infinite, `1`=play once, `N`=play N times (strict BUZ-02 compliance)
- `loop_start` field: chirps (×10) play only on the FIRST pass; subsequent loops restart at digits
- `flight_states.c` calls `buzzer_play_code(code, 2)` — plays status code twice then stops [BUZ-02]
- `buzzer_set_code()` / `buzzer_set_altitude()` kept as inline shims for existing callers
- `hal_buzzer_task_register()` added to all three HALs; hardware HAL registers into `hw_tasks[]`
- Test and sim HALs drive the task via `hal_tasks_tick()` for full pattern testing
- **12 unit tests**: BUZ-PAT-01..07 (tone counts, BUZ-02 repeat, chirp-once), BUZ-ACT-01..03 (lifecycle, stop, repeat)
- **Hardware observation**: With 2.1.17, buzzer was garbled due to rapid false-launch state cycling.
  With 2.1.24+, buzzer correctly plays startup chirps + status code × 2 then goes silent.

### Pressure Processing Module ✅ Done — Hardware Verified
- `pressure_processing.h/.c` — standalone IIR filter + hypsometric altitude module
- `pp_init()`, `pp_feed(raw_pa, now_ms)`, `pp_read(&sample)` — ring-buffered producer/consumer
- `pp_start_cal()` / `pp_cal_sample()` / `pp_end_cal()` — boot calibration pipeline
- `pp_test_prime(ground_pa)` — test-only helper to skip calibration
- `pp_filter_pressure()` / `pp_pressure_to_altitude_cm()` — public utility functions
- All detectors consume altitude via `pp_read()` — single data path from sensor to state machine
- `hal_tasks_tick()` feeds `pp_feed()` at ~50Hz (matching real BMP280/MS5607 sample rate)
- **Hardware verified**: v2.1.24 shows stable 102,260–102,300 Pa readings with no overflow

### Batch Pressure Processing (v2 Task 8) ✅ Done
- `flight_process_samples(ctx, batch)` — processes a 5-sample 50Hz batch in one call
- `detect_ascent()` / `detect_descent()` gate at 20ms (50Hz); `detect_pad_idle()` gates at 10ms
- `main_hardware.c` calls `dispatch_state()` via `pp_read()` ring (v2.1.24+)
- Polled fallback (`dispatch_state()`) retained for test/sim compatibility

### Fire-and-Forget Flight Log (v2 Task 9) ✅ Done
- `hal_log_start(cfg, ground_pa)` arms the log at launch and `hal_log_stop()` closes it at landing; core0 creates and writes `flight_log.csv` inside its flash window
- `hal_log_sample(time_ms, pa, alt, state, thrust, event)` appends one CSV line — non-blocking
- Three implementations: hardware (LittleFS streaming), sim (host filesystem), test (in-memory)
- Incremental ring-buffer CSV logger (`csv_flush_safe/step/track`) retired from all call sites

### Data Logging
- 4096-sample ring buffer, events tagged on samples
- Streaming CSV export to littlefs after landing
- /api/flight.csv serves actual flight data

### Web Interface
- 4-tab UI: Status, Config, Flight Data, Update
- Pending config warning, range validation, guided editor
- GitHub Pages demo + interactive WASM simulation

### Simulator
- Pyro black box (sim/main_sim.c) — no physics knowledge
- CLI physics driver (sim/sim_cli.c)
- Browser simulation (docs/sim.html) with Web Audio buzzer

### Pyro Fault Detection
- AP2192 FLAG pin monitoring (GPIO 17/18, active-low with pull-ups)
- Post-fire continuity verification (ADC re-check 500ms after fire)
- Fault events logged to flight buffer
- Beep codes 2-3/2-4 (P1 fault/verify) and 3-3/3-4 (P2 fault/verify)

### Ground Test Commands (v2 Task 2) ✅ Done
- Serial commands via TRRS jack (UART0 RX)
- 3-second ARM→FIRE window with automatic disarm timeout
- All commands rejected outside PAD_IDLE state

### Network Diagnostics (v2.1.25–26) ✅ Done
- `net_glue.c`: rx/tx/drop counters on every frame, `tud_network_init_cb()` logged
- `http_server.c`: **Fixed `conn_alloc()` bug** — was scanning 4 of 8 pool slots; now uses all 8
- `http_server.c`: HTTP accept counter for connection tracking
- `main_hardware.c`: 10-second `NET:` health heartbeat
- Periodic `$PYRO` state sentences suppressed on UART (events still sent)

### Testing (107 C + 22 web)
- 43 unit, 22 integration, 15 closed-loop, 12 buzzer, 15 config
- 4 safety-critical closed-loop tests
- 22 Playwright web UI tests
- cppcheck/MISRA, clang-format, pmccabe in CI
- Requirements traced to integration tests (TRACEABILITY.md)

### Build & CI
- Firmware, tests, simulator, WASM all from same source
- GitHub Actions on every push
- Auto-versioning, A/B OTA images

## 🔨 Known Issues

### RESOLVED: HTTP Server Not Accepting Connections (v2.1.26) ✅
- **Symptom**: `http=0` in all NET diagnostics; `txf` grows rapidly; ARP incomplete
- **Root cause**: **MAC address mismatch** between USB descriptor string (`STRID_MAC = "020284006A00"`)
  and `tud_network_mac_address` array (`{0x02,0x02,0x84,0x6A,0x96,0x00}`). The host's ECM driver
  expected device MAC `02:02:84:00:6A:00` (from descriptor), but lwIP sent ARP replies with source
  MAC `02:02:84:6A:96:01` (from array + XOR). Host rejected responses as invalid source.
- **Fix**: Aligned `tud_network_mac_address` in `net_glue.c` to match `STRID_MAC`:
  `{0x02, 0x02, 0x84, 0x00, 0x6A, 0x00}`
- **Verified**: HTTP 200 in 24ms, `/api/status` returns valid JSON, device up in 3s after flash

### RESOLVED: False Launch on Boot ✅
- Fixed in v2.1.24 via `pressure_processing.c` dt_ms clamp
- **Hardware verified**: 8 boot cycles on 2.1.17 showed false launches; 4 boot cycles on 2.1.24+ showed none

### RESOLVED: conn_alloc() Pool Bug ✅  
- Fixed in v2.1.25 — was scanning 4 of 8 pool slots
- May not have been the primary HTTP issue (see HTTP failure above)

### Minor: Playwright Reboot Cycle Test
- Skipped in CI (needs log access)

## 🚧 v2 Architecture Task Status

| Task | Description | Status |
|------|-------------|--------|
| v2-1 | X-macro config system (`config_fields.h`) | ✅ Done |
| v2-2 | HAL config API + Ground test commands | ✅ Done |
| v2-3 | CPU sleep (`hal_sleep_until_event`) | ⏸️ Disabled v2.1.27 |
| v2-4 | Serial readline HAL | ✅ Done |
| v2-5 | Autonomous pressure sampling | ✅ Done |
| v2-6 | Telemetry formatter module | ✅ Done |
| v2-7 | Buzzer pattern player (async task) | ✅ Done + HW verified |
| v2-8 | Batch flight_process_samples() | ✅ Done |
| v2-9 | Fire-and-forget hal_log_sample() | ✅ Done |
| v2-10 | ISR UART TX ring buffer | 🔄 Replaced by v2-13 |
| v2-11 | Network diagnostics + conn_alloc fix | ✅ Done |
| v2-12 | Fix HTTP TX failures (MAC mismatch) | ✅ Done v2.1.27 |
| v2-13 | DMA UART TX (replaces v2-10 ring buffer) | ✅ Done v2.1.28 |

## 🔨 Next Priority
1. **Flight machine rework** — phase-based descent, emergency deploy, mach gate,
   5 s rolling ground level, remove the backup apogee timer. Design in hand,
   not yet started.
2. **Export / import Lua programs** — no way to get a script off a board or onto
   another one today. `/api/lua/script` serves the raw text, so this is mostly a
   UI affordance plus a sensible filename.
3. **Convert MK1B's `ms5607_read()` fallback** — the last `sleep_ms()` on a flight path, 11 ms in stage 3
4. **Instrument `ota_flush()`** — separate erase from program timing to explain MK1A's 1.0 s/sector
5. **Re-enable WFE sleep** — MAC mismatch was the real root cause, not `__wfe()`
6. Long-duration soak test (network + UART stability over hours)
