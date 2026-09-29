# FreeRTOS, SD storage and the high-rate logger

Started 2026-09-29 on branch `freertos-sd`. The user's instruction:

> build the mac with the RNG. Do a full refactor to the freertos plan.
> Continue until all tests pass and the application does not lockup. Add
> drivers for SD and accelerometer. Develop a logger that uses the SD card
> for high rate logs. Use the SD for all files, but copy the SD card
> configuration files to the littlefs storage when they are different. Run
> until you reach the limits of the SD card performance or the full
> high-rate logger works over high altitude flights.

The design is `docs/log_storage_plan2_freertos.md` (plan 2), mode 0
(`configRUN_MULTIPLE_PRIORITIES` 0), as the user directed on 2026-09-28.

## Decisions taken here (plan 2, section 13, and what it left open)

| Question | Taken | Why |
|---|---|---|
| Plan 1 or 2 | 2 | the user |
| Scheduling mode | 0, network on core1 with Lua | the user |
| Discards or the gap (5.4) | discards (DD-068 already does it) | no schedule coupling between the logger and the sensor |
| Runaway script | preempted, instruction budget kept | Lua's hook already bounds a tick |
| Who writes flash | any task but the flight task, each operation raised to T under a persistent per-core lockout | plan 2's 4.2 rule 1 is about the priority at the moment of the lockout; one writer task would serialise every file request through T and stop the flight task for each |
| The logger's priority | P, on core1 | an SD card's busy time is hundreds of ms; at T it would stop the flight task for all of it |
| SD boards | a build-time variant, `mk1c_sd` | pins belong to the board; J3 is SPI0 |
| FreeRTOS version | V11.3.1, FetchContent | the latest release |
| FatFs | R0.16, vendored under `lib/fatfs` | ChaN publishes no repository |

## Tasks (plan 2 section 3, as built)

| Task | Core | Priority | Does |
|---|---|---|---|
| lockout helper x2 | 0 and 1 | T | parks its core with interrupts off for a flash operation |
| flight | 0 | P | the 20 ms step on a hardware-timer alarm; feeds the watchdog |
| net | 1 | P | TinyUSB, lwIP, HTTP |
| lua | 1 | P | the VM |
| storage | 1 | P | the flight log, queued file work, the SD, the high-rate log |
| timer daemon | 1 | P | the SDK's interop |

## Phases

Each phase ends with every host suite passing, all four boards building,
`prove_core0.py` / `wait_check.py` / `trace_check.py` passing, a commit and
a push.

- [ ] A. MAC from the RNG (DD-072)
- [ ] B. FreeRTOS in the build; the superloop as the flight task; net and
      Lua as tasks; the flash window, core1 units and the kill removed;
      flash operations under the lockout (DD-073, DD-074)
- [ ] C. Bench: all five boards, G4 checks, a soak with no lockup
- [ ] D. `mk1c_sd`: SPI0, the SD driver, FatFs, the storage layer, SD for
      every file, config mirrored into littlefs (DD-075, DD-076)
- [ ] E. LSM6DS3 driver; the high-rate logger on the SD (DD-077)
- [ ] F. A bench flight source for high-altitude flights; measure the SD
      and the logger to their limits
- [ ] G. RAW-INT for the flight log on boards without an SD, if time allows

## Progress notes

(appended as phases land)
