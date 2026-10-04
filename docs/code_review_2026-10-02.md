# Full Code Review — 2026-10-02

Reviewed at `main`, VERSION 2.2.3, HEAD `3c9eb89`. Read-only; no source changed.

**Scope:** all first-party code:
- `src/**`, including Lua, which the 2026-09-24 review left out
- `boards/**`, `sim/**`, `www/`
- `support/`, `scripts/`, `.github/workflows`, the CMake build
- the test suites
- the requirements, traceability and design documents

**Two questions:**
1. **Correctness and safety.** Does the code do what it must, above all on the paths that fire a charge?
2. **The comment rule.** Comments are a smell. Code structure and naming should tell the story. A comment exists to:
   - give a subject-matter expert the non-obvious decision, or
   - trace a requirement.

   A long explanation belongs in a design document, or in a primary source outside the project, that the comment points to.

**Method.** Seven parallel reviews, one per area, then a consolidation pass.

| Tag | Meaning |
|---|---|
| **verified** | Re-read against the source at HEAD during consolidation |
| **reproduced** | Shown by a host program: littlefs path traversal, Lua panics and budget bypasses, MS5607 error, config parsing |
| **plausible** | Reasoned from code and documents, not reproduced |

The pico-sdk and ARM toolchain were not available, so nothing was run on target.

---

## 1. Summary

| ID | Sev | Finding | Status | Requirement |
|---|---|---|---|---|
| CR-01 | **Critical** | A fire pulse is stamped on the live clock but checked against the loop's stale `now`. On MK1B the pulse can end immediately; on MK1C the precharge times out and all later fires are refused. | verified | PYR-SAFE-01, PYR-ARM-* |
| CR-02 | **Critical** | Lua `flight.*` state constants (0..6) do not match `flight_state_t` (`PAD_IDLE`=3 … `LANDED`=8). A script acting on `flight.DROGUE` acts during ASCENT. | verified | — (none exists) |
| CR-03 | **High** | The emergency ladder can fire channel 2 when it is configured Disabled. | verified | FLT-EMRG-01, CFG-04 |
| CR-04 | **High** | The deploy altitude is stored as `(uint16_t)atoi()` with no range check. A UI unit switch can silently wrap 700 m to 44.64 m. | verified | CFG-*, WEB-UI-02 |
| CR-05 | **High** | `POST /www/../config.ini` overwrites any littlefs file, bypassing the PAD_IDLE interlock and validation. | reproduced | WEB-API-*, PYR-SAFE-04 |
| CR-06 | **High** | OTA has no upper bound on the image size, so it erases past the download slot into littlefs. | verified | OTA-01 |
| CR-07 | **High** | A failed config read (I/O error, lock timeout) overwrites `config.ini` with defaults, and the board flies them. | verified | CFG-* |
| CR-08 | **High** | `config.ini` parsing does not trim. `pyro1_mode=delay␠` becomes NONE, and `pyro2_value␠=` is ignored. The file is truncated at 511 bytes. | reproduced | CFG-08 |
| CR-09 | **High** | MS5607 second-order temperature compensation (datasheet p.9) is not implemented: about 290 Pa error at 0 °C, 1.2 kPa at −20 °C. | verified | SNS-PRES-* |
| CR-10 | **High** | MS5607 PROM CRC-4 (AN520) is never checked, so a corrupt coefficient read is accepted for the whole flight. | verified by reading | SNS-PRES-* |
| CR-11 | **High** | MK1A/MK1B log every good fire as "did not open". The verify window reads a pre-fire continuity sample, and the ladder can re-fire the spent drogue. | plausible (host model) | PYR-VERIFY-01, PYR-REFIRE-02 |
| CR-12 | **High** | The MK1C SDK header maps `PICO_DEFAULT_SPI_CSN_PIN` to FIRE_A (GPIO17) and `SPI_RX` to BIAS_A (GPIO16). | verified | — |
| CR-13 | **High** | The wrong-board-image guard runs only after an OTA. A UF2 or picotool flash of the wrong board's image runs on the wrong pin map. | verified by reading | — |
| CR-14 | **High** | USB RX stalls permanently when one `pbuf_alloc` fails (`tud_network_recv_cb` returns true with nothing held). | verified | — |
| CR-15 | **High** | Lua host-side calls outside `lua_pcall`, such as `lua_getglobal("tick")` under a strict-mode `_G`, reach `abort()` and halt core1. | reproduced | — |
| CR-16 | **High** | The Lua instruction budget can be defeated by `pcall` in a loop, by `__gc` finalisers, and by exponential patterns of 64 bytes or less. | reproduced | — |
| CR-17 | **High** | Lua C-stack sizing is too small for `LUAI_MAXCCALLS`=200. `lua_check` has a 6.4 kB frame on the 8 kB net stack and on the 2 kB boot stack. | plausible (host measured) | — |
| CR-18 | **High** | The `LUA_32BITS` override is a no-op. Lua runs with 64-bit ints and soft doubles, against a 32 kB arena. | reproduced | — |
| CR-19 | **High** | A failed OTA is reported as success by `app.js`, `update_from_release.py`, `install.py` and `upload_fw.sh`. | verified by reading | WEB-UI-05 |
| CR-20 | **High** | `install.py` always prefers the MK1C image and does not check `board_id`. | verified by reading | — |
| CR-21 | **High** | Safety test oracles are circular or empty. Details below. | verified | PYR-SAFE-03/04 |
| CR-22 | Medium | CSRF / DNS rebinding: any web page can POST `/api/ota` (CORS `*`, no Origin/Host check). | plausible | — |
| CR-23 | Medium | SD endpoints bypass the in-flight filesystem lock and can re-mount the volume under the flight log. | verified by reading | WEB-API-08 |
| CR-24 | Medium | TX MTU includes the Ethernet header, so a 1501–1514 B datagram overruns the TinyUSB epbuf. | plausible | — |
| CR-25 | Medium | `/api/status` is no longer a one-pass snapshot (core1 reads core0 fields). The Lua seqlock fallback returns a torn copy. | verified by reading | WEB-API-11 |
| CR-26 | Medium | A stale "valid" mark on the OTA slot plus a later partial OTA lets the bootloader swap in a corrupt image. | plausible | — |
| CR-27 | Medium | A firmware commit failure is never retried, so the OTA image silently rolls back on a later boot. | verified by reading | — |
| CR-28 | Medium | A flight log that cannot open or drain holds the filesystem lock until reboot. Every file API returns 423, and the storage-stall reset is defeated. | verified by reading | FLT-LOG-* |
| CR-29 | Medium | Lazy, unsynchronised mutex creation in `littlefs_driver.c` and `hal_common.c` after the scheduler starts. | plausible | — |
| CR-30 | Medium | `hal_serial_readline` wedges after 63 bytes with no newline. MK1A's half-duplex echo can feed `$PYRO` back into the ground-test parser. | verified / plausible | GND-TEST-* |
| CR-31 | Medium | I2C bus recovery drives SDA/SCL push-pull high, which fights a target holding SDA low (UM10204 §3.1.16 needs open drain). | verified by reading | — |
| CR-32 | Medium | MK1A/MK1B `pyro_fire()` accepts a second fire mid-pulse. MK1B also does not range-check the channel (index −1). The interlock rests only on the caller. | verified | PYR-DEPLOY-02 |
| CR-33 | Medium | A scrubbed flight leaves a valid pad marker, so a later power-up while descending (e.g. in an elevator) can recover "armed, apogee seen". | plausible | FLT-BOOT-* |
| CR-34 | Medium | `on_event()` is never called on hardware (`lua_app_event` has no caller). | verified | — |
| CR-35 | Medium | Lua software "PWM" runs at 0.2 Hz with a maximum duty of 39%. | verified by reading | — |
| CR-36 | Medium | `pyro_bridge.pio` cannot hold a level: each word gives an 8–16 ns pulse, then the dead band. | verified by reading | — |
| CR-37 | Medium | The sim atmosphere is isothermal from 11 to 47 km, so it is wrong above 20 km (−30 % at 47 km). Three copies exist. | reproduced | TST-05, SIM-02 |
| CR-38 | Medium | The sim GPIO plant ignores pin direction. A missing `gpio_set_dir(OUT)` on a fire pin passes in sim. | verified by reading | — |
| CR-39 | Medium | Supply chain: `pico_fota_bootloader` is fetched at `master`, and actions are pinned by tag (third-party actions included). | verified | — |
| CR-40 | Medium | A local build bumps VERSION, so a developer image claims the next release number. | verified by reading | — |

The Low findings are collected in §5.

### What is in good shape

- **Lua isolation.** Bytecode is refused, unsafe libraries are not linked, pins are reached only by name, and Lua cannot block flash or starve core0.
- **Flash lockout sequencing.**
- **Integer maths and constants**, checked against their primary sources:
  - SD CRC7/CRC16 and CSD maths
  - BMP280 compensation (Bosch §3.11.3)
  - MS5607 first-order compensation
  - LSM6DS3 register values
  - PIO UART/WS2812 timing
- **Earlier review items** REV-06, REV-17, REV-22 and REV-27 (code part) are fixed.
- **Board `.c` files.** Their comment ratios are 3–17 %: DD-060's "structure in place of comments" worked there.

---

## 2. Critical and High findings: detail

### CR-01: one fire, two clocks
`src/main_hardware.c:224` takes `now = hal_time_ms()` once per loop. Within the same loop:
1. `dispatch_state()` → `fire_channel()` → `hal_pyro_fire()` stamps the pulse start from the **live** clock (`boards/mk1b/pyro_board.c:141`, `boards/mk1c/pyro_sequence.c:89`).
2. Then `flight_update_outputs(ctx, now)` (`main_hardware.c:263`) → `pyro_update(now)` tests `now - pulse.start_ms >= FIRE_PULSE_MS` unsigned (`mk1b/pyro_board.c:155`; `mk1c/pyro_sequence.c:109`).

If STAGE 2 and dispatch cross a millisecond boundary, `start = now + 1`, the subtraction wraps to about 4.29e9, and:
- **MK1B:** the pulse ends microseconds after it starts.
- **MK1C:** `FAULT_PRECHARGE_TIMEOUT` latches, and every later fire is refused.

In both cases the flight log says ENERGISED. MK1A uses a signed `deadline_reached` and is not affected. The host tests use one clock, so they cannot see this.

**Fix:**
- Pass `now` through `hal_pyro_fire(ch, now)`.
- Use `deadline_reached()` for every elapsed-time test.
- Add a test in which the fire clock leads the update clock by 1 ms.

### CR-02: Lua state numbers
`src/lua/pyro_lua.c:658-665` publishes `BOOT=0 … LANDED=6` under the comment "Values match flight_state_t". The enum (`src/flight_states.h:35-50`) is `BOOT_SETTLE=0, BOOT_CONTINUITY, BOOT_CALIBRATE, PAD_IDLE=3, ASCENT=4 … LANDED=8, BOOT_SENSOR, FAULT, GROUND_TEST`. `lua_app.c:383` passes `ctx->current_state` through unchanged.

`docs/lua.html` and the simulator use the same wrong numbering, so neither catches it.

**Fix:** generate both the enum and the Lua table from one X-macro list. That structure removes the comment and the bug together. Add a test asserting `flight.LANDED == LANDED`.

### CR-03: the ladder fires a Disabled main
`src/flight_states.c:1177-1193` gates on `pyro2_continuity_good` (the raw reading, `:554`), but not on mode. `channel_expects_igniter()` (`:502`) already exists for exactly this check. This is the same class of bug as REV-02.

The ladder also assumes "channel 1 = drogue, channel 2 = main", and no requirement states that.

### CR-04: deploy altitude wraps at 65 535 cm
- **Firmware:** `src/config.c:145-149` stores the value with `(uint16_t)atoi(val)`; there is no range check in `config.c` or `http_server.c`.
- **UI:** `www/app.js:50` sets `MAX_ALT[cm] = 800000`, and `unitsChanged()` (`:303`) rescales without checking.

Main set to 700 m, then units switched to cm: the UI shows 70000, and the board stores 4464 cm.

**Fix:**
- Give each `CONFIG_FIELDS` row a min/max and parse with `strtol`.
- Reject out-of-range values rather than casting them.
- In the UI, limit cm to 65535 and refuse to save.

### CR-05: path traversal on upload
`http_server.c` routes `/www/*` with `dest = path` and then `rename(dest.part → dest)`; `vfs.c` checks no paths. littlefs resolves `..`. A host repro against littlefs v2.11.2 overwrote `config.ini`.

This bypasses the 409 interlock outside PAD_IDLE, CFG-06 merge, `serial_valid()` and `pin_store_save()`.

**Fix:** reject dot-segments, `//`, `\` and `%` inside `vfs_open/rename/remove`, so every caller gets the check.

### CR-06: OTA overrun
`ota_flush()` (`http_server.c:177-190`) advances `ota_offset` with no limit, and `route_post` (`:1536`) never compares `content_length` with the slot size. Only `app.js` filters by filename.

**Fix:**
- Return 413 when `content_length > slot`, taking the slot size from the pfb linker symbols.
- Guard inside `ota_flush`.
- Check the board tag before `pfb_mark_download_slot_as_valid`.

### CR-07 / CR-08: config integrity
**CR-07.** `hal_common.c:1031-1043` treats every `n <= 0` as "no file" and writes defaults. That includes −1 (I/O, mutex timeout, SD CRC) and −3 (`HAL_FS_LOCKED`). `flight_init` ignores the return value.
- On SD boards the mirror copies the defaults into littlefs too.
- `pin_store.c:35` and `board_identity.c:58-75` repeat the pattern.

**CR-08.** Two INI tokenisers with different whitespace rules (`config.c:185`, `pin_assign.c:380`) are the root cause. `lua_baud` is U16 (115200 → 49664).

**Fix:**
- Persist defaults only on −2 (ENOENT).
- Use one shared tokenizer.
- Reject a value that does not parse, and keep the previous value.

### CR-09 / CR-10: MS5607 accuracy and integrity
`src/ms5607_driver.h:39-49` implements page 8 only. The page-9 T2/OFF2/SENS2 flow applies whenever TEMP < 20 °C. The error is about 3 m AGL per °C of cooling near 0 °C, and the ground calibration removes only the constant part.

PROM detection checks `prom[0]`, a reserved word; the CRC-4 in word 7 (AN520) is never computed. Both findings cite the datasheet in `docs/datasheets/MS5607-02BA03_2017-06.pdf`. Add test vectors at 10, −10 and −20 °C.

### CR-11: post-fire verify reads a stale sample (MK1A/MK1B)
The window `500 < t < 600` ms (`flight_states.c:253-279`) runs in STAGE 3, before STAGE 4 ends the pulse. `pyro_sample()` is a no-op on every board, so `pyro_get` returns the pre-fire check, which reads good, and the verdict is NOPEN.

The retry rung (`:1166`) can then re-fire the drogue at +2 s. Writing `pyro1_fire_time` also restarts the 2 s grace, which delays a genuine main-forcing.

**Fix:** a board-reported post-fire verdict, unknown until a check completes after the pulse. Also delete the comment at `:263`, which describes a stimulus no board performs.

### CR-12: the MK1C SDK header
`boards/mk1c/sdk/pyro_mk1c.h:53-67` says "SPI (unused on this board…)" and sets `RX=16` (BIAS_A) and `CSN=17` (FIRE_A). The comment is false: `mk1c_sd` uses SPI0 on GPIO18–21. Any SDK or BSP code that uses the defaults hands the fire gate to SPI0.

**Fix:**
- Use RX=20 and CSN=21, or leave the defaults undefined.
- Add a `_Static_assert` that no `PICO_DEFAULT_*_PIN` equals a FIRE/BIAS/ARM pin.

### CR-13: board-image guard
`hal_firmware_commit()` (`hal_common.c:1467`) acts only when `pfb_is_after_firmware_update()`. `board_selftest.h:11-15` claims it covers a hand-flashed image, which it does not.

**Fix:** check the verdict at every boot. On FAIL, refuse to arm or fire, and announce it.

### CR-14: USB RX stall
`src/net_glue.c:162-184` returns `true` with `received_frame == NULL` in two cases: `pbuf_alloc` failed, or `size == 0`. TinyUSB then waits for `tud_network_recv_renew()`, which `net_service()` never calls. This is a candidate cause of the unexplained 40–60 s HTTP outages (DD G4-N).

**Fix:**
- Return `false` in those two cases.
- Bound the copy with `pbuf_take`.

### CR-15 – CR-18: Lua runtime
- **CR-15.** Several calls run outside `lua_pcall`: `pyro_lua.c:758,784,824,829` and `lua_newthread` at `:723`. A metamethod error or out-of-memory there reaches `on_panic`, which returns, so `abort()` follows: HardFault, and core1 spins. In flight this loses the log for the rest of the flight. The comment at `:105` ("returns to the host's setjmp") is false.
- **CR-16.** Three ways around the budget:
  - The hook's error is caught by the script's own `pcall`.
  - `__gc` runs with hooks off.
  - `PATTERN_DIRECT_MAX` 64 still allows exponential backtracking: 8.9 s on x86.
- **CR-17.** Set `LUAI_MAXCCALLS` to a measured value. Make `lua_check`'s `seen_t` arrays static. Measure the high-water mark on target.
- **CR-18.** `LUA_USER_H` is included at the end of `lua.h`, not `luaconf.h`, so the 32-bit selection has already happened. Patch `luaconf.h` and add `_Static_assert(sizeof(lua_Number)==4)`.

### CR-19 / CR-20: update tooling
- **`app.js:730`:** shows "Rebooting…" on both `.then` and `.catch`, without checking `r.ok`.
- **`update_from_release.py:173`:** catches `URLError`, a superclass of `HTTPError`, and returns `True`, so an HTTP error counts as success.
- **`install.py:55`:** has a bare `except: return True`.
- **`upload_fw.sh`:** runs curl without `--fail`.
- **`install.py:22`:** picks `mk1c` first, and looks for a legacy `.bin` name that releases no longer ship.

### CR-21: safety tests that cannot fail
| Location | Problem |
|---|---|
| `test/test_closedloop.c:849` | "No fire during ascent" checks the firmware's *own* state, not physics truth. A firmware that declares apogee early passes. |
| `run_suite` | Never asserts `fires <= 1` (PYR-SAFE-03, marked ✅). |
| `test/test_pressure_chain.c:355-476` | Six `test_T0_baseline_*` tests only `printf`. |
| `test_closedloop.c:1438` | `TEST_IGNORE`s itself on exactly the failure it guards. |
| `test_closedloop.c:398,886` | The rig clears `mock_pyro.firing` every tick and detects one fire per tick, so a double fire in one tick is invisible. |

---

## 3. Comment quality

### 3.1 Metrics

Measured as comment-only lines against code lines; vendored FatFs is excluded.

| Area | Comment : code |
|---|---|
| `src/` overall | 0.34 |
| `boards/` | 0.28 |
| Lua | 0.40 (29 % of lines) |
| `test/*.c` | 0.15 |

The heaviest files are headers that carry essays:

| File | Comment : code |
|---|---|
| `src/hal.h` | 2.19 |
| `src/pressure_processing.h` | 1.92 |
| `src/pin_assign.h` | 1.62 |
| `src/lua/lua_iface.h` | 1.60 |
| `src/async_task.h` | 5.1 |
| `pad_claim.h` / `pyro_release.h` | about 72–74 % |

Comment volume is concentrated in a few large files:

| File | Comment lines |
|---|---|
| `flight_states.c` | 421 |
| `hal_common.c` | 273 |
| `http_server.c` | 252 |
| `pyro_lua.c` | 170 |

**The density is in the wrong places.** Headers explain at length, while the files whose numbers are the real decisions are bare:
- `lwipopts.h`: 24 tuned constants, including the pool behind CR-14, and 0 % comments.
- `sim/physics.c`: no source for any physical constant.

### 3.2 Findings by category

| Category | Approx. count | Worst examples |
|---|---|---|
| **Stale or false** (most harmful) | ≥ 80 | `pyro_lua.c:105` (setjmp), `:658` (state values, CR-02), `pyro_luaconf.h:3` (CR-18), `mk1c/sdk/pyro_mk1c.h:53` (CR-12), `board_selftest.h:11` (CR-13), `flight_states.c:263` ("one stimulus"), `http_work.h:1-84` (describes the core0/core1 handoff removed by DD-073), `telemetry_formatter.h:51` (states 0–3, actually 0–5), `beep_store.h:7,19` ("thirteen codes", "nine keys"), `buzzer.c:113`, `hal.h:107,196,204`, `pad_claim.h:21` (names a function that does not exist), `lua_pio.pio:2-18`, `docs/core1_hazard.md` (cites deleted files), `CMakeLists.txt:42,1186`, `physics.h:23` |
| **History narration** (belongs in git) | ~70 across code, tests and CI | "used to"/"no longer"/"removed" (`hal_common.c:1096,1119`, `flight_states.c:617,1545`, `lua_platform_cfg.h:31`, `lua_app.c:110`, `lua_core1.c:193`, `http_server.c:571-582, 1181, 1380, 2035`, `main_hardware.c:49`, `pin_store.c:27`); plan and review tags cited as if they were requirements (`v2-8`, `v2-9`, `v2-10`, `N7`, `N11`, `N26`, `T5-A`, `G4-N`); test copy-paste (`test_config_persistence.c` ×5); `build.yml:117-123` |
| **Restates the code** | ~120 | `flight_states.c:1655-1674` (`/* Success */` return codes), `pressure_processing.c` section labels, `physics.h` doxygen one-liners, `app.js`, `update_from_release.py:202-257`, `usb_descriptors.c` |
| **Stands in for naming or structure** | ~60 | `0=none,1=ms5607,2=bmp280` (use `pressure_sensor_type_t`); units 0/1/2 (no enum); 14 parallel `pyro1_*`/`pyro2_*` fields with about 12 `ch==1 ? :` ternaries (use `pyro_channel_t pyro[2]`); the hand-coded `ts+1` hold sentinel in 6 places (use a `hold_t`); three copy-pasted descent detectors; `MK1B BOARD_PIN_PYRO_COMMON_EN` (the THEORY doc explains the misleading name; rename it `PYRO_LOW`); `tracking_*` vs "presence test"; `compat_state_id` (rename `state_to_telem_id`); local `extern`s instead of headers (10+); `"MUST be first"` casts (use `container_of`) |
| **Essays that belong behind a pointer** | ~45 | The `flight_states.c` Mach, descent and ladder blocks duplicate `docs/flight_states.md` sections that the code never points to. Others: `pyro_bridge.pio:1-45`, `board_selftest.h:1-48`, `board_identity.h` (birthday-bound maths), `flash_op.h` (duplicates DD-074), `hr_log.h` (a file-format spec, belongs in `docs/`), `pad_claim.h`/`pyro_release.h` (design records, belong in a DD), sim plant schematic walk-throughs (belong in THEORY), the "host reads it at enumeration" rationale written 4 times (point to DD-072 once) |
| **Non-obvious constant with no source** | ~90 | `FIRE_PULSE_MS 500`; verify window 500/600; landing 100/200/3000/1000 (these are FLT-LAND-01..03, uncited); `PACK_PRESENT_MIN_COUNTS 200`; `RAMP_MV_PER_MS 890` (TPS2595 dVdT); all of `lwipopts.h`; `uart_init(115200)`; `PYRO_LUA_ARENA_BYTES`, `PYRO_LUA_BUDGET`, `C1_STACK_WORDS`; `CMakeLists` slot `-128`; USSA76 constants in `sim/physics.c` (`9.81` vs `9.80665` elsewhere, `8500` m scale height); `plant.c:335` ADC 890 "≈27 °C" (wrong; RP2040 §4.9.5 gives 876) |
| **Misplaced or orphaned** | ~10 | `flight_states.c:299,490,249`; `ms5607_bus.h:32`; `pin_caps.c:50`; `pyro_lua.h:527`; `lua_platform_cfg.h:270` |

### 3.3 Pointers to documents

**Code delegates often.** It carries 129 `DD-` pointers, 35 `THEORY_OF_OPERATION` pointers and 238 requirement citations, and `trace_check.py` resolves the DD and `See X.md "Heading"` forms.

**The gaps:**
- **Unchecked pointers:** 15 of 38 THEORY pointers wrap across comment lines and are never checked. A heading rename breaks them silently. The fix is to join comment continuations before matching.
- **Pointers out of the repository:** 48+ citations reach `~/Documents/pyro_mk1c/DESIGN.md`, `Pyro_mk1a.pdf` or `*.kicad_sch` on the author's machine. Nobody else can follow them, and nothing can check them. Vendor the cited sections under `docs/`, or cite a released document with a revision.
- **Missing primary-source citations:** `i2c_recover.c` (UM10204 §3.1.16), `sim/physics.c` (NOAA-S/T 76-1562), `lwipopts.h` (lwIP), and the Lua allocator (Lua 5.4 §4.6).
- **Good models to copy:**
  - `mac_random.h:3-6` (datasheet section and page)
  - `http_conn.c:184,343` (RFC 9110 §15.5.6, 9112 §2.2)
  - `lsm6ds3.c` and `sd_card.c` (a page per register)
  - `mk1c/pyro_sequence.c:128` (one pointer covers eight constants)

### 3.4 Representative rewrites

```c
/* Before: src/flight_states.c:1199-1215 */
bool altitude_stable = abs(altitude - prev_altitude) < 100;
bool speed_low = abs(ctx->vertical_speed_cms) < 200;
bool near_ground = altitude < 3000;
... if (ts + 1u - ctx->landing_stable_since >= 1000)

/* After: names carry the meaning, comments carry the trace */
#define LAND_STEP_MAX_CM   100   /* FLT-LAND-01 */
#define LAND_SPEED_MAX_CMS 200   /* FLT-LAND-02 */
#define LAND_AGL_MAX_CM    3000  /* FLT-LAND-03 */
#define LAND_HOLD_MS       1000u /* FLT-LAND-01 */
bool resting = abs(altitude - prev_altitude) < LAND_STEP_MAX_CM &&
               abs(ctx->vertical_speed_cms) < LAND_SPEED_MAX_CMS && altitude < LAND_AGL_MAX_CM;
if (held(resting, &ctx->landing_stable_since, ts, LAND_HOLD_MS)) return true;
```

```c
/* Before: src/lua/pyro_lua.c:658 -- a comment asserting a fact the code violates */
/* Flight state constants... Values match flight_state_t; see flight_states.h. */
static const char *states[] = {"BOOT", "PAD_IDLE", "ASCENT", ...};

/* After: structure guarantees it; no comment needed */
#define X(name) {#name, name},
static const struct { const char *n; int v; } states[] = { FLIGHT_STATE_LIST(X) };
```

```c
/* Before: src/hal_common/hal_common.c:1096 */
/* Watchdog initialization removed - watchdog_reboot() handles enabling internally
 * when needed. The 1ms timeout was causing boot loops. */
/* After: delete. History is in git; the watchdog is armed in flight_task (DD-073). */
```

```c
/* Before: src/flight_states.c:1123-1137 -- 15-line ladder essay */
/* After */
/* [FLT-EMRG-01/02, PYR-REFIRE-01/02] See docs/flight_states.md "The emergency ladder". */
#define EMRG_DROGUE_GRACE_MS 2000u /* PYR-REFIRE-01 */
```

```c
/* Before: src/i2c_recover.c:7 */
#define CLOCK_EDGES 18u /* nine pulses */
/* After */
#define RECOVERY_SCL_PULSES 9u /* UM10204 Rev.7 §3.1.16 "Bus clear" */
/* ...and od_low()/od_release() helpers make the open-drain rule (CR-31) visible in the names. */
```

```c
/* Before: flight_states.c:1655-1674 */
return -1; /* Rejected: not in safe state */ ... return 0; /* Success */
/* After */
typedef enum { CFG_APPLIED, CFG_NOT_ON_PAD, CFG_LOAD_FAILED, CFG_INVALID } cfg_apply_t;
```

### 3.5 Structure that drives comment density

Each of these splits turns a section-banner comment into a file name or type name.

- **`flight_states.c`:** 1896 lines; `flight_context_t` has about 110 fields under banners. Split it into `flight_ring.c`, `flight_pyro.c`, `flight_boot.c`, `flight_descent.c` and `flight_mach.c`. Group the context into `mach_lock_t`, `descent_t`, `ladder_t` and `pyro_channel_t[2]`.
- **`hal_common.c`:** 1498 lines with about 14 responsibilities. Split it into `hal_pressure.c`, `hal_uart.c`, `hal_fs.c`, `hal_flight_log.c`, `hal_pyro.c` and `hal_platform.c`.
- **`http_server.c`:** 2102 lines. Split it into `ota.c`, `api_json.c` and `http_lwip.c`. Also:
  - Replace the 20-way `strcmp` chain in `serve_get` with a route table.
  - Delete the dead `http_work_*` worker path (WEB-HTTP-07 was withdrawn).
- **`CMakeLists.txt`:** the flight-core source list is copied into about 7 test targets and has already drifted (`buzzer.c`). Factor it into one variable.

---

## 4. Traceability

| Metric | Value |
|---|---|
| Requirements defined | 335 (7 withdrawn) |
| Live requirements not cited in any non-test code | 198 / 328 |
| Live requirements cited by no test | 227 / 328 |
| Dangling IDs | 11 (`BUZ-PAT-*`, `BUZ-ACT-*` in `test_buzzer.c`) |
| Withdrawn IDs cited as live | 2 (FLT-LAUNCH-06 `flight_states.c:601`; PYR-SAFE-02 via a range at `:151`) |
| TRACEABILITY ✅ rows naming no test or function | 68 |
| Duplicate rows | PWR-SLEEP-01 |

**Safety code with no requirement tag:**
- `boards/mk1c/pyro_sequence.c` and `arm_pump.*`: PYR-ARM-01..06, PYR-FIRE-01
- `beep_codes.c`: BUZ-CODE-*
- `landing_detected`: FLT-LAND-01..03
- `pressure_believed`: PYR-MODE-06
- `www/app.js`: WEB-UI-01..06
- `http_conn.c`: WEB-HTTP-01..06

**The Lua runtime has no requirement at all.** `pyro_lua.c:14` cites "L4, L5, L7, L8, L11 from REQUIREMENTS.md". Those are invariants in `thoughts/shared/plans/2026-09-21-…md`, and L1, L2, L8 and L10 no longer match the code. Promote them to `LUA-SAFE-xx` requirements.

**Requirement text and code disagree:**

| Requirement | Disagreement |
|---|---|
| FLT-BOOT-04 | Cited on code that does the opposite (DD-053) |
| FLT-RATE-03 | Logging is thinned, not sampling |
| DAT-03 | Events are separate rows |
| PYR-ALT-02 | Asks for a warning; the code beeps fatal |
| PWR-SLEEP-01 | ✅, but `hal_sleep_until_event` is a no-op |
| PWR-LOG-01/02/03 | Ring size, file open and format all differ |
| WEB-API-11 | Not a snapshot (CR-25) |
| WEB-HTTP-04 | Returns 400, not 411 |
| PYR-FAULT-01 | Holds on MK1B only |
| PYR-FAULT-03 | TRACEABILITY cites retired beep codes |

**What `support/trace_check.py` misses.** It reports 0 problems, but it proves only that cited IDs in *known families* exist. Add:
- unknown-family detection
- withdrawn-ID detection, including inside ranges
- "✅ needs an executable verifier"
- duplicate-row detection
- multi-line THEORY pointers
- validation of free-text `docs/` paths
- coverage of `.sh` and `.yml` files

---

## 5. Low findings (abridged)

### Flight
- `sample_continuity` `<= 1000` gives a 1020 ms period. PYR-CONT-01 says "at least once per second".
- A stale `under_thrust` is logged during descent.
- A corrupt state dispatches to PAD_IDLE.
- Torn `pressure_trace` reads.
- `pad_claim` caps the mask at GPIO29.
- NaN in `mach_round_clamp`.
- Brownout recovery waits the full 2.5 s settle, and `RECOVERY_DEADLINE_MS` (4 s) is shorter than `SENSOR_BRINGUP_MS` (5 s).

### Boards and buzzer
- MK1B check period is slightly over 1 s.
- Ground-test abort re-runs channel 1.
- Beep-out drops digits above 999 999, and is undefined at `INT32_MIN`.
- `beep.ini` `gap`/`repeat` wrap.
- An empty personality name plays Default.
- 30 ms chirps are quantised to the 20 ms tick.
- Dead `BOARD_PIN_SPARE_GPIO 22` (it is the IMU CS on `mk1c_sd`).
- `arm_pump.c` hard-codes `pio0`.
- `pyro_sample()` is empty on every board.
- `cmake_minimum_required(3.13)` but the build uses 3.18 features.
- `build_wasm.sh` cannot link `sim_mk1c`.

### HAL and storage
- `ms5607_bus.h` hard-codes `i2c1_hw`.
- SD `wait_ready` gives the bus back without holding it.
- CMD59 is sent with CRC off.
- `hr_log` short-write duplicates data.
- `FF_FS_LOCK 0` while the web path can unlink an open file.
- The BMP280 plausibility range is wider than the part's.
- `PARK_MAX_US` is cited only for W25Q128.
- `lfs_format` return value is ignored.

### HTTP
- Beep audition reports "playing" on a timeout.
- `apply_api_config` does not check for NULL.
- Backlog 8 has no effect without `TCP_LISTEN_BACKLOG`.
- `lwip_port_rand()` is milliseconds since boot.
- RST on a mid-body error.
- About 2.1 kB of locals in `serve_get`.

### Lua
- `pio_add_program` is called without `pio_can_add_program`.
- `c1_state` has two writers.
- `lua_baud == 0` divides by zero.
- Out-of-memory and partial claims in `lua_iface_publish` are not undone.
- The arena allocator never grows or splits in place, and is O(blocks).
- Dead `LUA_C1_PARKED`, `lua_core1_flash_ok`.

### Web, sim and tooling
- `app.js`:
  - Release `tag_name` is inserted unescaped.
  - `textContent = esc()` escapes twice.
  - Status polling stacks requests with no timeout.
  - `WEB_VERSION` is hard-coded to 2.0.0.
  - The `reboot_required` / `cfgFileSelected` / `luaSave` flows show the wrong pending state.
- `ping -t` is the TTL on Linux.
- picotool path is hard-coded.
- `register_board.py` needs `ifconfig`.
- CI:
  - cppcheck and clang-format can never fail, but "release every green build" relies on them.
  - The `patch-release` job has no `concurrency:` guard.
  - `build.yml` has no top-level `permissions:`.
  - `${{ }}` values are interpolated into shell.
- Sim:
  - Linear drag.
  - `replay.c` refuses CRLF logs.
  - `sim_cli --replay` with no path flies a 0 m flight.
  - The browser demo's `api_shim.js` is never loaded.

### Tests and documents
- Test names carry review and task IDs (`test_REV*`, `test_T5_clean`, `test_M0_report`) instead of behaviour.
- Stale facts in tests ("1000 cm launch gate").
- Three overlapping status logs. `STATUS.md` declares itself stale; `SESSION_NOTES.md` is a diary; `docs/outstanding_tasks.md` is 1497 lines. Retire them in favour of issues and git history.
- DECISIONS.md:
  - 2066 lines mixing decisions, measurement logs and open work.
  - No Status/Superseded field.
  - DD-013 is still filed as live after DD-022 removed it.
- `SPECIFICATION.md` opens with an "AI Restart Summary".
- "DD-001 to DD-071" in three documents, but DD-079 exists.
- Stray `UART_log.txt`, `flight_sim.csv`, and `docs/ground-station-interface-spec 2.rtf` at the repository root and in `docs/`.

---

## 6. Recommended order

1. **Before the next flight:**
   - CR-01 (one clock for the fire pulse)
   - CR-03 (ladder mode gate)
   - CR-04 / CR-07 / CR-08 (config range, parse, persistence)
   - CR-11 (fresh post-fire verdict)
   - CR-12 (MK1C SPI defaults)
   - CR-13 (boot-time board guard)
   - CR-09 / CR-10 (MS5607)
   - CR-02, if any flier uses Lua
2. **Network and update path:** CR-05, CR-06, CR-14, CR-19, CR-20, then CR-22 – CR-27.
3. **Test oracles (CR-21)**, so the fixes above are held by tests that can fail.
4. **Lua runtime hardening:** CR-15 – CR-18 and CR-34 – CR-36, then promote the invariants to `LUA-SAFE-xx`.
5. **Comment pass, worst first:**
   - Delete or fix the stale and false comments (§3.2, row 1). These actively mislead.
   - Delete the history narration.
   - Replace essays with pointers, and vendor the external `DESIGN.md` sections.
   - Add primary-source citations for constants.
6. **Structural splits (§3.5) and the `trace_check.py` hardening (§4).** These stop the comment debt from coming back.
