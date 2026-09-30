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

- [x] A. MAC from the RNG (DD-072) -- 907a67b
- [x] B. FreeRTOS in the build; the superloop as the flight task; net and
      Lua as tasks; the flash window, core1 units and the kill removed;
      flash operations under the lockout (DD-073, DD-074) -- 8974faa
- [x] C. Bench: the four reachable boards, G4 checks, no lockup (the first
      MK1C, with a drawn MAC, is not reachable: macOS makes no interface)
- [x] D. `mk1c_sd`: SPI0, the SD driver, FatFs, the storage layer, SD for
      every file, config mirrored into littlefs (DD-075, DD-076) -- b244b34,
      c4d196e; the card itself has not come up (SPI-1)
- [x] E. LSM6DS3 driver; the high-rate logger on the SD (DD-077) -- f5bbaa9;
      the IMU verified, the log on a card waits on SPI-1
- [x] F. A bench flight source for high-altitude flights; measure the SD
      and the logger to their limits (DD-078, DD-079; the card on its own
      regulator, C-U6; HA-1 found and left to the user)
- [ ] G. RAW-INT for the flight log on boards without an SD, if time allows

## Progress notes

(appended as phases land)

- 2026-09-29, B on the second MK1C (192.168.42.1): api_check 41/41,
  http_stream_check 16/16, hw_ui_check passed. Two boot failures found on the
  bench and fixed: guard words written into the Lua stack's bottom tripped
  the kernel's overflow check; pfb's commit copies a 4 kB sector onto the
  caller's stack (net and storage stacks now 8 kB). A committed crash-looping
  image was recovered by `picotool reboot -u -f --vid 0x2E8A --pid 0x4002
  --bus 2 --address <USB Address from ioreg -r -d 1 -n <node>>` in its alive
  window, then picotool load of bootloader and app.
- 2026-09-29, F: the bench flight on MK1C-SD. 1 km: drogue at apogee, main at
  300 m, LANDED, no overrun or refusal. 10 km: Mach lock set in the boost,
  released at 35.5 s, apogee on time. 30 km, first flight: the lock never
  released and the drogue fired from the fallback at 150 m; second flight:
  released 12 s before apogee. The host, with sensor noise, maps the limit
  (docs/high_altitude_flight.md, HA-1): the lock does not release above
  about 15 km at MK1C's 3 Pa, 22 km at 1.2 Pa. Before that the host found the
  descent ladder forcing the main at 29 km under a working drogue: fixed by
  judging rates in the pad's air (DD-079). The card still resets in ACMD41,
  so no high-rate log has been written on hardware.
- 2026-09-29, F finished: with a MIC2920A-3.3 for the card, 734 kB/s in
  4 kB writes at 12.5 MHz and 1020 at 20.8; a 30 km bench flight logged
  16.2 MB whole -- 1,088,033 IMU sets at an even 614.33 us, 33,421 pressure
  conversions with none missed (the flight log on the card, the internal
  flash unwritten), 6,684 snapshots. Found and fixed on the way: the ready
  wait before a command (read limit, not write-busy), the logger stranded by
  a remount or a failed write (HR-06), batches stamped before the bus.
