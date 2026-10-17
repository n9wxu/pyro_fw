# Appending to the flight log without erasing: five ways

**Status: design, 2026-10-14. Nothing here is implemented.**

The question: "Research 5 ways to avoid any FS append operations that will
cause pyro to break the loop time."

This builds on `docs/log_storage_options.md` (plan 1: the conflict classes,
goals G1-G5), `docs/log_storage_plan2_freertos.md` (plan 2, built: the
FreeRTOS tasks and `flash_op()`) and `docs/raw_flight_log.md` (the
pre-erased area). Figures are cited to `docs/datasheets/` (file, PDF page)
or to the source. Estimates are marked.

---

## 1. Rulings

Given while this was being written, 2026-10-14:

- "Fast writes require pre-erase. Before flight, we must ensure all unused
  flash is erased."
- "writing to a dirty sector is not allowed, we must only write to erased
  sectors. It does not matter if the write is metadata or document data.
  Erase before write adds significant delay to the append operations."

What they require of any design that logs to the QSPI flash:

| | Requirement |
|---|---|
| **F1** | Before flight, all unused flash is erased. |
| **F2** | No append erases: every write in flight, data or metadata, programs flash that is already erased. |

Read here as: a write may program the erased remainder of a sector that
holds earlier records, since those bytes are erased (every part allows a
partial page program, section 2). If "erased sectors" means whole sectors
never written since their erase, each 1 s sync takes a fresh 4 kB sector:
14.7 MB an hour at any log rate, all of an 8 MB filesystem in 34 minutes.
That reading is decision 1 in section 8.

What the rulings exclude:

- **littlefs 2.11.3 as it is.** After each sync the next append erases a
  block and copies the partial one into it (L2), and a metadata block that
  fills is compacted into its pair's other block, which is erased first.
- **littlefs v3 as it is.** With its block map and pre-erase, data blocks
  come pre-erased, but a metadata compaction still swaps to the other block
  of its pair and erases it in place: 6.4 to 15.5 erases a minute with
  `prog_size` 256 (simulated: the fork's `REQUIREMENTS.md`, Appendix B.1,
  LFS3-PERF-10).
- **Hiding the erase instead of removing it.** Slicing an erase with the
  flash's suspend and resume (section 6.2), or keeping the flight path in
  RAM through it (section 6.3), stops it breaking the loop, but the append
  still waits for the erase. Both are kept for F1's pre-erase, where the
  wait is harmless.

---

## 2. The problem in numbers

- **The loop is 20 ms** (`src/loop_period.h`). An overrun is a period whose
  alarm fires before the flight task has finished the last one
  (`src/main_hardware.c`, `loop_overruns`).
- **Every flash program or erase stops both cores.** It takes the QSPI flash
  out of execute-in-place, so `flash_op()` parks the other core in RAM with
  its interrupts off, disables the caller's, and runs the operation
  (`src/flash_op.h`, DD-074). The flight task is stopped for all of it.
- **littlefs 2.11.3 erases about once a second in flight** (L2). Measured on
  MK1C: `stage_max_us[7]` 46 to 73 ms, 20 to 77 overruns a minute at 50 Hz
  (DD-035).
- **Plan 1's goal G1:** "No log write delays the flight loop by more than a
  small bound (a millisecond or two), and none can stall it."

The flash parts:

| Board | Part | Page program, typ / max | 4 kB erase, typ / max | 64 kB erase, typ / max | Suspend | Source |
|---|---|---|---|---|---|---|
| MK1A | W25Q128JV | 0.4 / 3 ms | 45 / 400 ms | 150 / 2,000 ms | erase and program; 20 us to suspend | `W25Q128JV_RevH_2021-03-10.pdf` pp. 43, 66 |
| MK1B | W25Q16JV | 0.4 / 3 ms | 45 / 400 ms | 150 / 2,000 ms | as the W25Q128JV | `W25Q16JV_RevI_2024-12-24.pdf` pp. 40, 64 |
| MK1B-8MB | BY25Q64ES | 0.45 / 2.4 ms | 35 / 300 ms | 180 / 2,000 ms | erase only; 30 us to suspend | `BY25Q64ES_Rev2.9_2024-10-29.pdf` pp. 62, 71 |
| MK1C, MK1C-SD | XT25F128F | 0.4 / 2 ms | 40 / 800 ms | 250 / 1,600 ms | erase and program; 20 us to suspend, 500 us at least from resume to suspend | `XT25F128F-W_Rev0.6_2022-02-26.pdf` pp. 43, 55-56 |

All four program fewer than 256 bytes "without having any effect on other
bytes within the same page" (W25Q16JV p. 34, W25Q128JV p. 37, XT25F128F
p. 36, BY25Q64ES pp. 56-57). The RP2040 SDK programs whole pages, so a
partial program is a page of 0xFF around the new bytes. None of the four
limits how often one page may be programmed so.

The log's volume, from 22-byte rows (`FLOG_SAMPLE_BYTES`,
`src/flight_log.h`) and the plans of `src/log_plan.h`, for a 10-minute
flight with six events (estimates):

| Plan | Rows | 10-minute flight |
|---|---|---|
| `1hz` (the default, `src/config_fields.h`) | one a second, and each event row | about 13 kB |
| `events` | the same, and every 20 ms sample within 1 s of an event, about 100 an event | about 26 kB |
| `full` | every 20 ms sample | about 660 kB |

MK1C-SD already logs the flight to its SD card while one is mounted, and
its internal flash only without one
(`boards/mk1c_sd/THEORY_OF_OPERATION.md`, DAT-09).

---

## 3. The five ways at a glance

Each meets F2: nothing in the append path erases.

| # | Way | Per sync in flight | Metadata in flight | Changes |
|---|---|---|---|---|
| W1 | A pre-erased raw area outside littlefs | one partial-page program | none: records carry a length and CRC | the flash map |
| W2 | A pre-built chained file inside littlefs | one partial-page program | none: the chain is linked on the pad | a new file type in littlefs |
| W3 | littlefs v3 with every compaction moved to a pre-erased block | metadata commits, about 200-768 B | appends to erased space only | the littlefs v3 fork |
| W4 | An FRAM or MRAM on its own SPI bus | one SPI write, no busy time | none | the boards |
| W5 | An SD card on its own SPI bus (MK1C-SD's way) | a write to a RAM ring, drained by a task | the card's | the boards; built on MK1C-SD |

W1 to W3 need F1's pre-erase, and section 6 shows how to do it without
breaking the loop. W4 has nothing to erase. W5's card erases on its own
time, behind its own bus.

---

## 4. The five ways

### W1. A pre-erased raw area outside littlefs

`docs/raw_flight_log.md` in full. A fixed, sector-aligned region at the end
of the flash map holds the log; littlefs keeps everything else. Records are
framed with a length and a CRC, as the SD card's high-rate log's are
(DD-077, `src/sd/hr_log.h`), after a header naming the flight. The end is
the first record that is erased or fails its CRC, so a boot finds where a
log stopped without metadata, and a resumed flight (DD-086) appends after
it.

- **In flight:** one partial-page program a second: 0.4 ms typical, 2 to
  3 ms at most. Only the worst case exceeds G1. The Winbond and XTX parts
  can suspend a program; the BY25Q64ES cannot.
- **F1:** the area is erased on the ground (section 6), and the pass's
  completion recorded in littlefs, so a pass a power cut interrupts is
  redone.
- **Cost:** a flash-map change (`docs/raw_flight_log.md` D1): littlefs
  shrinks and its files move once. A second storage path, read through the
  existing renderer (D5). Our own code, not a filesystem's.
- **Leaves:** a program disturbs the pressure conversion beside it (DD-068):
  one a second.

### W2. A pre-built chained file inside littlefs

The log stays a littlefs file, of a new kind. On the pad, the blocks a
flight needs are erased and linked: each block's header names the next. The
file's metadata names the first block once, at open. In flight the log
only programs records into the chain; its size is found by walking it, a
header a block, and scanning the last block for the last record whose CRC
holds. After landing, one commit records the end and frees the unused
blocks.

- **In flight:** as W1.
- **F1:** the chain is the pre-erase, and it is also the pre-allocation:
  the flight's blocks are reserved before launch.
- **Cost:** a new file structure in the littlefs fork: traversal and the
  allocator must follow the chain, `lfs3_fs_ck` must check it, and it needs
  its own power-loss tests. A disk format change. A chain cannot be
  rewritten in place, so a block that fails to program needs a way around
  it, such as several next-block slots, the last written winning.
- **Gain over W1:** no flash-map change and no fixed size.

### W3. littlefs v3, every compaction moved to a pre-erased block

littlefs v3 already appends data into pre-erased blocks and metadata into
the erased tail of its metadata blocks. What erases in its append path is
compaction: a full metadata block swaps to the other block of its pair and
erases it. v3 can already move a pair instead, allocating a new one, when
the block has been reused `block_recycles` times or a program fails
(`lfs3_mdir_alloc___()`, `lfs3.c` near line 10773 on `v3-integration`,
58367256). A mode for flight would take that path for every compaction,
from the block map's pre-erased blocks.

- **In flight:** each sync commits about 200 B with `prog_size` 1, or about
  768 B with 256 (the fork's `REQUIREMENTS.md`, Appendix B.2): two to three
  page programs, 0.4 ms each typical. A compaction becomes a copy into a
  pre-erased pair, about every 5 to 20 syncs, and its parent's commit.
- **F1:** `lfs3_fs_gc()` with `LFS3_GC_PREERASE` erases every free block on
  the pad and records them in the block map; a mount with
  `LFS3_M_REVPERTURB` trusts them across a reboot.
- **Cost:** a fork change, and v3 itself: alpha, a new disk format, and a
  migration of every board (LFS-3). The mroot anchor, blocks 0 and 1,
  cannot move (`lfs3.h`, `block_recycles`), so a compaction there must be
  shown never to fall in a flight, for example by compacting on the pad. To
  be verified in the code: that B-tree nodes compact into new blocks rather
  than in place.
- **Gain:** a general filesystem that meets F2 for every file, not only the
  log; useful upstream.

### W4. An FRAM or MRAM on its own SPI bus

An F-RAM writes each byte as its eighth clock ends, with no erase, no
write delay and no polling (`FM25V20A_001-90261_RevJ_2023-12-22.pdf`
pp. 2, 14). A 22-byte row is 27 bytes on the bus with its opcodes: about
7 us at 31.25 MHz. Nothing touches the QSPI flash, so nothing parks a core:
plan 1's class NONE.

| Part | Size | Clock | Endurance | Retention | Write current | Flights at `events` / `1hz` | DigiKey, 2026-10-14 |
|---|---|---|---|---|---|---|---|
| Infineon CY15B104QN | 4 Mbit | 50 MHz | 10^15 | 10 y at 85 C | 2.4 mA at 40 MHz | 19 / 39 | $20.61, in stock |
| RAMXEED MB85RS4MT | 4 Mbit | 40 MHz | 10^14 (p. 17) | 10 y at 85 C | 1.8 mA | 19 / 39 | $19.46, in stock |
| Infineon FM25V20A | 2 Mbit | 40 MHz | 10^14 | 10 y at 85 C | 2.4 mA | 9 / 19 | $22.33, in stock |
| Everspin MR25H40 (MRAM) | 4 Mbit | 40 MHz | unlimited | over 20 y | 28 mA typ, 42 max | 19 / 39 | $30.74, in stock |

Sources: `CY15B104QN_002-19436_RevO_2023-06-05.pdf` pp. 1, 24, 27;
`MB85RS4MT_DS501-00053-3v1_2025-07.pdf` pp. 1, 13, 17;
`FM25V20A_001-90261_RevJ_2023-12-22.pdf` pp. 19-20, 24;
`MR20H40_MR25H40_Rev12.6_2020-08-07.pdf` pp. 1, 17. The `full` plan,
1,100 B/s, fills 4 Mbit in 8 minutes; only the 8 Mbit CY15B108QN holds a
full-rate flight. No datasheet rates shock or vibration.

- **Board fit:** only MK1C has a whole SPI port on a connector: SPI0 on J3
  (GPIO18-21) and a second chip select on J1.6 (GPIO22), the pads Lua uses
  today. J3's 3.3 V is U6, the XC6206 that also feeds the MS5607: an
  F-RAM's 2-3 mA is harmless there, the MRAM's 42 mA write peak is not
  (`XC6206_ETR0305_004b.pdf`). MK1A and MK1B have free SPI pins on the
  RP2040 (MK1A SPI0 GPIO2-5, SPI1 GPIO12-15; MK1B SPI0 GPIO2-5, SPI1
  GPIO11-14) but none on a connector: a respin.
- **Leaves:** a hardware change for every board but MK1C, and a log format
  of its own (W1's records suit it).

### W5. An SD card on its own SPI bus

MK1C-SD's design, built: the flight log goes to the card while one is
mounted (DAT-09), and the high-rate log is written by a task from a 32 kB
RAM ring into a file preallocated and contiguous on the pad (`f_expand`,
`src/sd/hr_log.h`, DD-077). The card erases inside itself, on its own time;
the bus is SPI0, not the QSPI flash, so the flight task is untouched. On the
30 km bench flight, 33,421 conversions and none missed
(`boards/mk1c_sd/THEORY_OF_OPERATION.md`).

- **Leaves:** a card's own write latency, which the ring absorbs; a card's
  retention and its supply (MK1C-SD's card needed its own regulator, C-U6);
  and the internal flash as the fallback without a card, which then needs
  W1, W2 or W3.

---

## 5. Also considered: hold the log in RAM until landing

Rows stay in RAM from launch to landing and are written after it, when an
erase's delay no longer matters. SRAM survives a watchdog reset: the
RP2040's bootrom jumps to code pre-loaded in SRAM when the watchdog scratch
registers say so (`rp2040-datasheet_2025-02-20.pdf` p. 130), so a buffer
the C runtime does not clear survives the reset DD-086 resumes from. A power
cut loses it all, which FLT-LOG-06 forbids ("no more than the last second
shall be lost"). And 13 to 26 kB is about all the spare RAM the Lua boards
have, 13.5 kB; MK1C-SD has 44 kB (section 6.3). Not one of the five.

---

## 6. F1: erasing all unused flash before flight

### 6.1 How long it takes

Erasing every block of the filesystem, the upper bound (the files take
about 100 kB of it):

| Board | littlefs | 4 kB sectors: typ / max | 64 kB blocks: typ / max |
|---|---|---|---|
| MK1A | 8,192 kB | 2,048: 92 s / 13.7 min | 128: 19 s / 4.3 min |
| MK1B | 984 kB | 246: 11 s / 98 s | 15 blocks and 6 sectors: 2.5 s / 32 s |
| MK1B-8MB | 4,096 kB | 1,024: 36 s / 5.1 min | 64: 12 s / 2.1 min |
| MK1C, MK1C-SD | 8,192 kB | 2,048: 82 s / 27 min | 128: 32 s / 3.4 min |

Estimates from section 2's times. A 64 kB erase stops the system for
150-250 ms typically and up to 2 s, longer than the 1000 ms watchdog (L1),
unless sliced (6.2). After the first pass only freed blocks need erasing
again: a file rewritten, a flight deleted.

### 6.2 Slicing the erase with suspend and resume

Every part on the boards suspends a sector or block erase (section 2). An
erase becomes a series of short `flash_op()` slices:

1. leave execute-in-place;
2. start the erase, or resume it (7Ah);
3. wait out the slice in RAM;
4. suspend (75h) and poll the status until BUSY is 0 and SUS is 1;
5. flush the XIP cache and re-enter execute-in-place through the boot2
   copy.

Between slices the flight task runs from flash; the chip holds the erase.
Each stop is the slice, about 1 ms, plus the suspend (20-35 us), the cache
flush (about 8 us, `rp2040-datasheet_2025-02-20.pdf` p. 124) and the
misses while the 16 kB cache refills. The XT25F128F needs 500 us at least
between a resume and the next suspend (p. 55), so MK1C's slices are at
least that.

- **No SDK call does it.** `flash_range_erase()` runs the whole erase, and
  `flash_do_cmd()` re-enters execute-in-place after every command, which
  must not happen while the chip is busy (pico-sdk 2.2.0,
  `src/rp2_common/hardware_flash/flash.c`). The routine is ours, in RAM,
  built from the bootrom calls `flash.c` uses.
- **While an erase is suspended,** no other erase and no status-register
  write may start (W25Q16JV p. 40): `flash_op()` holds every other writer,
  the OTA path included, until it finishes.
- **Unknown until measured:** whether an erase cut into hundreds of slices
  completes in its rated time. The datasheets give the suspend latency and
  the least resume-to-suspend gap, nothing more.
- **DD-068:** a slice beside a pressure conversion disturbs it. Slices
  spread one a period touch one conversion each; bunched into a few
  periods' slack, few.

At one 1 ms slice a period, a typical 45 ms sector takes 0.9 s, and all of
MK1A's filesystem about 31 minutes; slices filling most of each period's
slack make it a few times faster. The pad is too slow for a whole first
pass; the bench and after landing are not.

### 6.3 Keeping the flight path in RAM through the erase

Plan 1's R1 (`docs/log_storage_options.md`, section 2): with the flight
task, its interrupt handlers and the kernel in RAM, an erase stops only the
work in flash, and the flight goes on. Measured 2026-10-14 by a static call
graph of the 2026-10-13 ELFs, from `flight_task`, the core0 interrupt
handlers and every function table they reach (`support/prove_core0.py`'s
roots):

| Board | Flight-path code, steady state | Its read-only data | Already in RAM | Spare SRAM |
|---|---|---|---|---|
| MK1A | 51.9 kB | 3.3 kB | 2.4 kB | 13.5 kB |
| MK1B | 52.6 kB | 4.2 kB | 2.4 kB | 13.5 kB |
| MK1B-8MB | 52.3 kB | 4.2 kB | 2.4 kB | 13.5 kB |
| MK1C | 57.2 kB | 3.4 kB | 2.4 kB | 13.4 kB |
| MK1C-SD | 54.1 kB | 2.8 kB | 2.4 kB | 44.3 kB |

"Steady state" leaves out the sensors' bring-up, the task's setup and the
panic paths; with them, and the calls other tasks run on the flight task,
it is 60-65 kB. Spare is the newlib heap's gap below the stacks (about
9.3 kB, of which Lua's number formatting and stdio take some) and the free
2 kB at the bottom of each scratch bank. So RAM execution is short by 45 to
53 kB on the Lua boards and 13 to 18 kB on MK1C-SD. The room would have to
come from the largest users: lwIP's connections (42 kB), heap (20 kB)
and buffer pool (18 kB), and Lua's arena (32 kB), stack (16 kB) and script
checker's scratch (16 kB).

Beyond room, four things stand in the way:

- **What it would carry:** `snprintf` for the telemetry line (6.3 kB, the
  largest single item, cheaper to take off the flight path than to move);
  the SDK's float and divider shims (`PICO_FLOAT_IN_RAM` and its siblings
  move 1.7 kB; `float_math.c` ignores them); newlib's `memmove` and string
  functions, which the SDK's rule for keeping them in RAM misses on this
  toolchain (its archive members are `libg.a(libc_a-memmove.o)`); the
  FreeRTOS kernel, 4.2 kB, which core1 shares; and the switch tables of
  `buzzer_tick` and three others, which stay in flash unless built with
  `-fno-jump-tables`.
- **How to place it:** about 350 functions. `__not_in_flash_func` on each,
  with `__noinline`, is impractical; moving whole object files costs about
  100 kB, since files like `hal_common.c` mix flight and other code. A
  generated linker fragment naming the `.text.<function>` sections
  (`-ffunction-sections` is on) is the workable route.
- **Scheduling:** `flash_op()` runs at priority T, which under
  `configRUN_MULTIPLE_PRIORITIES` 0 stops every task at P on both cores,
  the flight task included (`src/flash_op.h`). RAM execution needs that
  changed too.
- **DD-068:** the flight goes on through the erase, but the conversions
  beside it are still discarded.

RAM execution is the costliest way to make F1's pre-erase harmless, and it
buys nothing for F2.

### 6.4 When and where

- **On the bench, with USB attached** (DD-058: USB means not flying): whole
  64 kB erases, unsliced; the loop's overruns there matter to nothing.
- **After landing:** the blocks the flight freed, and the next flight's.
- **At boot and on the pad:** sliced (6.2), what is left.
- **A gate:** C6 already holds OK to LAUNCH until the log is open and its
  header written; F1 adds that the erase is complete. Which blocks are
  erased must be known, not read: a block an interrupted erase left reads
  as 0xFF and may not hold a program. littlefs v2 records nothing (and
  erases before every new block anyway); v3's block map records pre-erased
  blocks, trusted only with revision perturbation (`lfs3.h`,
  `gc_preerase_count`); W1's area records the end of each erase pass in
  littlefs.

---

## 7. Comparison

| | W1 raw area | W2 chained file | W3 v3, compaction moved | W4 FRAM/MRAM | W5 SD card |
|---|---|---|---|---|---|
| Longest stop per sync | 0.4 ms typ, 2-3 ms max | as W1 | 2-3 page programs | about 7 us a row, no stop | none (a RAM ring) |
| Erases in flight | none | none | none | none to make | inside the card |
| Needs F1's pre-erase | the area | the chain | every free block | no | for the fallback |
| Survives a power cut | to the last sync | to the last sync | to the last sync | every row written | to the last write |
| Software | ours, small; flash map moves | littlefs fork, new file type | littlefs fork, v3 migration | a driver and W1's format | built (MK1C-SD) |
| Hardware | none | none | none | MK1C: none (J3); others: respin | MK1C-SD only |
| Upstream | n/a | perhaps | yes | n/a | n/a |

---

## 8. Decisions for you

1. **"Erased sectors":** may a write program the erased remainder of a
   sector that holds earlier records (assumed here), or must each write
   start a sector never written since its erase (a sector a second, 14.7 MB
   an hour)?
2. **"All unused flash":** every free block of the filesystem, as F1 reads,
   or only the space the flight can write (W1's area, W2's chain)?
3. **Which way, and for which boards:** W1 for today's boards with no
   hardware change; W2 or W3 to keep the log a littlefs file; W4 on MK1C's
   J3 and on the next respins; W5 where a card is fitted.
4. **Where the first pre-erase runs:** on the bench only, refusing OK to
   LAUNCH until it is done, or sliced on the pad as well.

---

## 9. Sources

- `docs/datasheets/` (index and notes in its `README.md`):
  `W25Q128JV_RevH_2021-03-10.pdf` pp. 37, 43, 66;
  `W25Q16JV_RevI_2024-12-24.pdf` pp. 34, 40, 64;
  `XT25F128F-W_Rev0.6_2022-02-26.pdf` pp. 36, 43, 55-56;
  `BY25Q64ES_Rev2.9_2024-10-29.pdf` pp. 56-57, 62, 71-72;
  `rp2040-datasheet_2025-02-20.pdf` pp. 124, 130, 238, 546;
  the F-RAM and MRAM datasheets of W4; `XC6206_ETR0305_004b.pdf`.
- pico-sdk 2.2.0, `src/rp2_common/hardware_flash/flash.c`.
- The littlefs fork, `v3-integration` 58367256: `lfs3.c` near line 10773,
  `lfs3.h` (`block_recycles`, `gc_preerase_count`), `REQUIREMENTS.md`
  Appendix B.1-B.2, `benches/bench_wlog.toml`.
- This repository: `src/flash_op.h`, `src/loop_period.h`,
  `src/main_hardware.c`, `src/flight_log.h`, `src/log_plan.h`,
  `src/sd/hr_log.h`, `boards/mk1c_sd/THEORY_OF_OPERATION.md`,
  `docs/raw_flight_log.md`, `docs/log_storage_options.md`; DD-035, DD-058,
  DD-068, DD-074, DD-077, DD-086; L1, L2, C6, LFS-3.
- DigiKey prices and stock, 2026-10-14.
