---
date: 2026-04-04T00:00:00-07:00
researcher: Joseph Julicher
git_commit: 111637357a337d93f1c1f3f3953d3e908eff2717
branch: main
repository: pyro_fw
topic: "Why is the LED not toggling at 5 Hz"
tags: [research, codebase, led, pressure, hal, batch, front-ready]
status: complete
last_updated: 2026-04-04
last_updated_by: Joseph Julicher
---

# Research: Why Is the LED Not Toggling at 5 Hz

**Date**: 2026-04-04
**Researcher**: Joseph Julicher
**Git Commit**: `111637357a337d93f1c1f3f3953d3e908eff2717`
**Branch**: main
**Repository**: pyro_fw

## Research Question

Why is the LED not toggling at 5 Hz?

## Summary

The LED toggle (`gpio_xor_mask(1u << 25)`) is placed inside a condition that is only
true when `p->back.count >= HAL_PRESSURE_BATCH_SIZE && !p->front_ready`. After the
very first batch of 5 samples is promoted, `front_ready` is set to `true` and is
**never reset to `false`** — because the function that resets it,
`hal_pressure_fifo_release()`, has no callers anywhere in the codebase. As a result
the LED toggles exactly **once** (~100 ms after boot) and then never again.

## Detailed Findings

### The Toggle Condition (`hal_hardware.c:145–150`)

The LED toggle lives inside `pres_append()`, gated by the batch promotion condition:

```c
if (p->back.count >= HAL_PRESSURE_BATCH_SIZE && !p->front_ready) {
    p->front = p->back;
    p->front_ready = true;
    p->back.count = 0;
    gpio_xor_mask(1u << 25);   /* proof-of-life: ~5 Hz heartbeat */
}
```

Both sub-conditions must be true simultaneously:
1. `p->back.count >= HAL_PRESSURE_BATCH_SIZE` — back buffer has accumulated 5 samples
2. `!p->front_ready` — the front buffer slot is free (consumer has released it)

### State Trace After Boot

| Event | `back.count` | `front_ready` | Condition | LED |
|---|---|---|---|---|
| Boot | 0 | false | — | — |
| Samples 1–4 arrive | 1–4 | false | false | — |
| Sample 5 arrives | 5 | false | **true** | **toggles once** |
| Promotion executes | 0 | **true** | — | — |
| Samples 6–10 arrive | 1–5 | true | **false** (gated by `!front_ready`) | — |
| `back.count` reaches 5 again | 5 | true | false | — |
| Sample 11 arrives | 5 (frozen) | true | false | — |
| All subsequent samples | 5 (frozen) | true | false | — |

### Why `back.count` Freezes at 5

Once `back.count == HAL_PRESSURE_BATCH_SIZE` (5), the sample-append guard at
`hal_hardware.c:135` prevents any further writes:

```c
int idx = p->back.count;
if (idx < HAL_PRESSURE_BATCH_SIZE) {   // 5 < 5 → false when count==5
    ...
    p->back.count++;
}
```

`back.count` can only be reset to 0 inside the promotion block (line 148), which
requires `!p->front_ready`. Since `front_ready` stays `true`, `back.count` freezes
permanently at 5 and no new samples are stored in the back buffer.

### Why `front_ready` Is Never Reset

`front_ready` is reset to `false` in exactly one place:

```c
// hal_hardware.c:277–279
void hal_pressure_fifo_release(void) {
    pres.front_ready = false;
}
```

A codebase-wide search for callers of `hal_pressure_fifo_release` finds **zero
call sites** outside its own definition. The function is defined but never invoked.

### The Actual Data Path (Bypasses the Batch Mechanism)

The flight software does not use the `front`/`back` batch mechanism at all.
The real data flow is:

```
pres_tick() → pres_append() → pp_feed() → pp.ring[] (pressure_processing.c)
                                                   ↓
                              pp_read() in flight_states.c (50 Hz)
```

`pp_feed()` is called directly from `pres_append()` at `hal_hardware.c:132` for every
individual sample at 50 Hz. The `pressure_processing.c` ring buffer (`pp.ring[]`,
size 32) is what the flight state machine actually reads via `pp_read()`.

The batch `front`/`back` mechanism in `pres_task_t` is a separate output pathway. It
accumulates batches of 5 samples into `pres.front`, but since `hal_pressure_fifo_release()`
has no callers, `front_ready` is never cleared after the first promotion.

### `hal_pressure_fifo_get()` Call Sites

`hal_pressure_fifo_get()` — the companion read function — is also uncalled:

```
grep -rn "hal_pressure_fifo_get" src/ → (no matches outside its own definition)
```

Both `hal_pressure_fifo_get()` (`hal_hardware.c:270`) and `hal_pressure_fifo_release()`
(`hal_hardware.c:277`) are defined but have no callers. The batch API is entirely unused
at runtime.

## Code References

- `src/hal_hardware.c:125–151` — `pres_append()`, contains the LED toggle and batch promotion
- `src/hal_hardware.c:134–140` — back-buffer append guard (`idx < HAL_PRESSURE_BATCH_SIZE`)
- `src/hal_hardware.c:145–150` — batch promotion condition (`!p->front_ready` gate)
- `src/hal_hardware.c:270–275` — `hal_pressure_fifo_get()` — defined, zero callers
- `src/hal_hardware.c:277–280` — `hal_pressure_fifo_release()` — defined, zero callers
- `src/pressure_processing.c:174–181` — `pp_read()` — reads from internal ring, not batch API
- `src/flight_states.c:246,290,335,388` — `pp_read()` call sites in flight state machine

## Architecture Documentation

The `pres_task_t` struct contains two distinct output pathways for sensor data:

1. **`pp_feed()` path** (active): Each raw sample at 50 Hz is fed directly into the
   `pressure_processing.c` ring buffer via `pp_feed()`. This is the live data path
   used by the flight state machine.

2. **Batch path** (`front`/`back`, inactive): Every 5 samples are accumulated into
   `p->back`, then promoted to `p->front` when the consumer releases `front_ready`.
   No consumer currently calls the release function, making this path permanently
   stalled after the first batch.

## Related Research

- `thoughts/shared/research/2026-04-04-web-interface-30min-timeout.md`
