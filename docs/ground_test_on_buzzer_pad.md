# The ground test switch across the buzzer pad

How a ground test switch whose driven pad is also the buzzer's is read, so
that MK1A, with two user pads, can carry both (GND-TEST-12, DD-087). Option A
below is built; its bench check, the list at the end, is owed
(`docs/outstanding_tasks.md`, R-4).

## What exists

The pair wiring (`ground_test=pair`, `ground_test_switch.h`) drives one pad
and reads the other. Each loop the driven pad flips, the read pad's pull flips
the other way, and the switch counts as closed only when the read pad has
followed the drive both ways. A read pad touching ground or 3V3 follows one
way only. The read is `gts_tick()` in `src/hal_common/hal_common.c`, a task
run once a loop (10 ms, `LOOP_PERIOD_MS`), on the same core and in the same
loop as the buzzer task.

Where the driven pad is the buzzer's, `gts_tick()` puts the level under test
on it for the read alone and puts the buzzer's own level back
(`read_under_a_pulse()`, option A). The buzzer keeps its own pattern.

`pin_assign_validate()` accepts the buzzer's pad as the driven pad of a pair
(`check_ground_test()`). It refuses the buzzer's pad as the read pad and as
the pad of a switch to ground (`check_gt_pad()`, `PIN_ERR_GT_BUSY`). Both the
buzzer and the switch are flight-software pads, so the claim model
(`pad_claim.h`) is satisfied.

## The idea

Keep the pair logic, but present the drive level only for an instant: pulse
the buzzer pad to the test level, sample the read pad, restore the buzzer's
own level. A pulse of nanoseconds to a few microseconds, 100 times a second,
is below what any buzzer turns into sound.

### Why a short pulse is enough

- **Closed:** the read pad is push-pull driven through the switch. It follows
  the drive edge in the pad delay plus the input synchroniser, roughly three
  `clk_sys` cycles, under 100 ns at 125 MHz. Its pull (about 50 kΩ) is
  irrelevant.
- **Open:** the read pad sits at its pull, which was set at the previous
  tick, one loop earlier. It is already settled. The pulse does not reach it.

So the only settling the pulse must cover is the drive edge itself, which is
why no timed wait is needed as long as the edge is fast.

### What slows the edge: the buzzer

The MK1A buzzer is operator-supplied, so its load is unknown.

| Load on the buzzer pad | Edge | Pulse needed | Sound from 100 pulses/s |
|---|---|---|---|
| Active buzzer module, transistor or logic input (1–10 kΩ) | tens of ns | under 100 ns | none: the oscillator needs milliseconds to start |
| Bare piezo element on the pad, 10–30 nF | 2–5 µs to a valid high at 12 mA | 5–10 µs | at most a faint tick; about 0.1 µJ per edge |
| Piezo through a series resistor or transistor | as the module | under 100 ns | none |

A pulse long enough for a bare element needs a timed wait, which DD-053 and
`wait_check.py` forbid in the loop. That is what separates the options.

## Options

### A. SIO pulse, no wait (recommended first step)

Built: `read_under_a_pulse()`, called from `gts_tick()` with the read pad's
pull already set opposite to the coming pulse.

1. remember the drive pad's current output level (`gpio_get_out_level()`);
2. write the pulse level;
3. read the read pad twice and keep the second: the first is the pad's
   synchroniser catching up with the edge;
4. write the remembered level back.

Pulse width about 50–100 ns, to be confirmed on a scope (below). Works for
every buzzer with an electronic input. With a bare piezo element the read pad
never follows and the switch reads open: a safe failure. No timing code, no
PIO, no new module. `gt_switch_step()` compares the read with the level under
test, the same comparison it makes for a driven pad of the switch's own.

While a tone plays, the buzzer pad is high and the pulse is a 100 ns dropout:
inaudible on any buzzer. The buzzer task and `gts_tick()` run in the same
loop, so the restore cannot race a tone change.

### B. PIO-timed pulse

Not built. A state machine on the pyro PIO drives the pulse, waits a
programmed count, samples the read pad into the RX FIFO and restores the
level; the loop reads the FIFO next tick. The pulse can be 5–10 µs with no CPU
wait, so a bare piezo element follows too.

Cost: the buzzer pad's function becomes PIO, so buzzer on/off must go through
the same state machine (a level register, or `set pins` from the CPU side).
`board_buzzer_on()` and `board_buzzer_off()` move behind the PIO for this
board. More code, a state machine, and a second way to drive the buzzer to
keep right. Worth it only if bare piezo elements must be supported.

### C. A with diagnosis

Not built: neither `/api/pins/caps` nor `/api/status` carries a follow count.
Option A plus a per-pad "followed high / followed low" count on one of them,
so a load that is too slow is seen on the bench rather than discovered as a
switch that never closes. Small, and useful regardless of A or B.

### D. Keep the exclusion, document it

Not chosen. No code: MK1A with a buzzer uses `ground_test=ground` to J6.5, and
the switch must be open at power-up. An operator wires the switch to the
buzzer's pad naturally, and the ground wiring cannot tell a switch to the
buzzer's driven-low pad from a switch to ground.

## Changes common to A, B and C

Built with A:

- `check_ground_test()`: in pair wiring, the driven pad may be the buzzer's,
  whether it is the board's own buzzer pad (MK1B, MK1C) or one assigned in
  `pins.ini`. The read pad stays exclusive (`check_gt_pad()`).
- `hal_ground_test_configure()` leaves the drive pad alone when it is the
  buzzer's: the buzzer owns its direction and idle level.
- `test_PIN_GT_08_the_switch_may_join_the_buzzers_pad_to_another` accepts the
  buzzer's pad as the driven one and refuses it as the read pad and as a
  switch to ground.
- `/api/pins` and `/api/config` are accepted in GROUND_TEST as in any state
  on the ground; what they store takes effect at the next start (CFG-10).

## Bench verification

MK1A, active buzzer on GPIO18, switch across J6.1/J6.2, `ground_test=pair`
with read 19 and drive 18:

1. held closed at power-up, the switch puts the board in GROUND_TEST (`state`
   on `/api/status`), and opening it starts the countdown; open at power-up,
   the board goes to PAD_IDLE; with the read pad jumpered to ground or 3V3 it
   never starts in GROUND_TEST;
2. nothing audible from the pulses, with and without a tone playing;
3. a scope on GPIO18 shows the pulse under 200 ns and the tone unaffected;
4. a bare piezo element in place of the module: the switch reads open;
5. `wait_check.py` stays at zero.

## Ruled 2026-10-02 (requirements review)

The user's rulings, which GND-TEST-12 states:

- The switch may connect the buzzer's pad to another pad. It may not ground
  the buzzer: `ground_test=ground` on the buzzer's pad stays refused.
- Pad to pad, the input pad watches the buzzer's own pattern. The switch is
  closed when the input follows the buzzer output's state.
- A series of short pulses may be used to find the switch's position, as
  options A and B above do when the buzzer is silent.
