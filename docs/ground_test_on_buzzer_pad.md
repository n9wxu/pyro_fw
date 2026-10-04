# The ground test switch across the buzzer pad

Options for reading a ground test switch whose driven pad is also the buzzer,
so that MK1A, with two user pads, can carry both. Written 2026-10-02 after the
bug report in which a buzzer on GPIO18 and a switch across GPIO18/19 could not
be configured together, and the fallback (one pad to ground) read the buzzer's
idle low as a closed switch at every power-up.

## What exists

The pair wiring (`ground_test=pair`, `ground_test_switch.h`) drives one pad
and reads the other. Each loop the driven pad flips, the read pad's pull flips
the other way, and the switch counts as closed only when the read pad has
followed the drive both ways. A read pad touching ground or 3V3 follows one
way only. The read is `gts_tick()` in `src/hal_common/hal_common.c`, once a
loop (20 ms, `LOOP_PERIOD_MS`), on the same core and in the same loop as the
buzzer task.

`pin_assign_validate()` refuses a switch pad that is the buzzer pad
(`check_gt_pad()`, `PIN_ERR_GT_BUSY`). That is the only thing in the way:
both the buzzer and the switch are flight-software pads, so the claim model
(`pad_claim.h`) is already satisfied.

## The idea

Keep the pair logic, but present the drive level only for an instant: pulse
the buzzer pad to the test level, sample the read pad, restore the buzzer's
own level. A pulse of nanoseconds to a few microseconds, 50 times a second,
is below what any buzzer turns into sound.

### Why a short pulse is enough

- **Closed:** the read pad is push-pull driven through the switch. It follows
  the drive edge in the pad delay plus the input synchroniser, roughly three
  `clk_sys` cycles, under 100 ns at 125 MHz. Its pull (about 50 kΩ) is
  irrelevant.
- **Open:** the read pad sits at its pull, which was set at the previous
  tick, 20 ms ago. It is already settled. The pulse does not reach it.

So the only settling the pulse must cover is the drive edge itself, which is
why no timed wait is needed as long as the edge is fast.

### What slows the edge: the buzzer

The MK1A buzzer is operator-supplied, so its load is unknown.

| Load on the buzzer pad | Edge | Pulse needed | Sound from 50 pulses/s |
|---|---|---|---|
| Active buzzer module, transistor or logic input (1–10 kΩ) | tens of ns | under 100 ns | none: the oscillator needs milliseconds to start |
| Bare piezo element on the pad, 10–30 nF | 2–5 µs to a valid high at 12 mA | 5–10 µs | at most a faint tick; about 0.1 µJ per edge |
| Piezo through a series resistor or transistor | as the module | under 100 ns | none |

A pulse long enough for a bare element needs a timed wait, which DD-053 and
`wait_check.py` forbid in the loop. That is what separates the options.

## Options

### A. SIO pulse, no wait (recommended first step)

In `gts_tick()`, with the read pad's pull already set opposite to the coming
pulse:

1. remember the drive pad's current output level (`gpio_get_out_level()`);
2. write the pulse level;
3. read the read pad after a short fixed dependency, for instance two reads
   with the second kept, or a handful of `nop`s;
4. write the remembered level back.

Pulse width about 50–100 ns. Works for every buzzer with an electronic input.
With a bare piezo element the read pad never follows and the switch reads
open: a safe failure, and `/api/status` can show the follow count so the
operator sees it. No timing code, no PIO, no new module. `gt_switch_step()`
is unchanged: its "drive level since the last step" becomes "the level at
the sample", which is the same comparison.

While a tone plays, the buzzer pad is high and the pulse is a 100 ns dropout:
inaudible on any buzzer. The buzzer task and `gts_tick()` run in the same
loop, so the restore cannot race a tone change.

### B. PIO-timed pulse

A state machine on the pyro PIO drives the pulse, waits a programmed count,
samples the read pad into the RX FIFO and restores the level; the loop reads
the FIFO next tick. The pulse can be 5–10 µs with no CPU wait, so a bare
piezo element follows too.

Cost: the buzzer pad's function becomes PIO, so buzzer on/off must go through
the same state machine (a level register, or `set pins` from the CPU side).
`board_buzzer_on()` and `board_buzzer_off()` move behind the PIO for this
board. More code, a state machine, and a second way to drive the buzzer to
keep right. Worth it only if bare piezo elements must be supported.

### C. A with diagnosis

Option A plus a per-pad "followed high / followed low" count in
`/api/pins/caps` or `/api/status`, so a load that is too slow is seen on the
bench rather than discovered as a switch that never closes. Small, and useful
regardless of A or B.

### D. Keep the exclusion, document it

No code. MK1A with a buzzer uses `ground_test=ground` to J6.5, and the switch
must be open at power-up. Rejected in practice: the bug report is an operator
doing the natural thing, and the ground wiring cannot tell a jumper to a
driven-low pad from ground, which is how the board ended up in GROUND_TEST
with every save path refusing.

## Changes common to A, B and C

- `check_gt_pad()`: in pair wiring, allow `gt_drive_pin` to equal the buzzer
  pad. The read pad stays exclusive. A board buzzer pad (MK1B, MK1C) qualifies
  as well as an assigned one, so this is not MK1A-only.
- `hal_ground_test_configure()`: do not `gpio_init()` the drive pad when it is
  the buzzer's; the buzzer owns its direction and idle level.
- Host tests in the pin assignment suite for the new acceptance and the
  still-refused read pad.
- `boards/mk1a/THEORY_OF_OPERATION.md`: the buzzer and the pair switch share
  GPIO18; which buzzer types work; the switch must be open at power-up.
- The lockout found in the same report, separately: accept `/api/pins` and
  `/api/config` in GROUND_TEST, which is grounded by definition.

## Bench verification

MK1A, active buzzer on GPIO18, switch across J6.1/J6.2, `ground_test=pair`
with read 19 and drive 18:

1. the switch open and closed shows in `/api/status` within 40 ms (two
   follows), and the pair never reads closed with the read pad jumpered to
   ground or 3V3;
2. nothing audible from the pulses, with and without a tone playing;
3. a scope on GPIO18 shows the pulse under 200 ns and the tone unaffected;
4. a bare piezo element in place of the module: the switch reads open (A) or
   closed (B), as the option predicts;
5. `wait_check.py` still at zero.

## Ruled 2026-10-02 (requirements review)

- The switch may connect the buzzer's pad to another pad. It may not ground
  the buzzer: `ground_test=ground` on the buzzer's pad stays refused.
- Pad to pad, the input pad watches the buzzer's own pattern. The switch is
  closed when the input follows the buzzer output's state.
- A series of short pulses may be used to find the switch's position, as
  options A and B above do when the buzzer is silent.
