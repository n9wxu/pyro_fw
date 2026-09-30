# A Pre-Erased Flight Log On The QSPI Flash

Status: options for a decision (task C6, plan phase G). 2026-09-29.

The question: can the flight log be a 2 MB area, erased before flight and
aligned with the flash's sectors, so that flight only programs it?

Yes, outside littlefs, or inside a patched one (below). Unpatched, no:
littlefs has no preallocation, and it
never writes into a block a committed file points to. Every append after a
sync allocates a fresh block, erases it and copies the partly filled last
block into it (`lfs_ctz_extend()`, `build/_deps/littlefs-src/lfs.c` line
2921, reached from `lfs_file_flushedwrite()` once `lfs_file_flush()` has
cleared `LFS_F_WRITING`, line 3420). Overwriting a placeholder file of 0xFF
is the same copy-on-write. That is why the flight log's once-a-second sync
(DD-035, FLT-LOG-06) costs an erase a second.

## Or: extend littlefs

The alternative to a raw area is to take the cost out of littlefs itself,
with two patches to littlefs 2.11.2. The disk format does not change: an
unpatched littlefs reads what they write. No layout change, no reformat, and
the flight log stays a file.

**Patch 1: append in place.** After a sync, the next append finds the
file's last block (`lfs_ctz_find()`) and then erases a new block and copies
the partial one into it (`lfs_ctz_extend()`), because littlefs cannot assume
the rest of the block is still erased. On a NOR flash that allows programming
the rest of a partly programmed page (W25Q128JV, PDF page 37), the append
can go on in the same block: the cache is loaded with the partial program
unit's existing bytes, and the write carries on from where the file ends.
Nothing committed is touched -- the directory still says the old size until
the next sync -- so a power cut mid-append leaves the committed file whole.
A block whose tail is not erased, because a power cut left bytes past the
committed size, fails the program's read-back, and littlefs already answers
`LFS_ERR_CORRUPT` from a data program by copying the block to a fresh one
(`lfs_file_relocate()`): the fallback is the cost we pay today, once. The
flash driver programs only bytes still 0xFF, so no bit is programmed twice.

With it, an erase happens only when a 4 kB block fills: at a row a second,
one every 3 minutes instead of every second; at every sample, one every
3.7 s.

**Patch 2: reserve.** A call that takes N free blocks for an open file and
erases them -- on the pad, paced, as D2 -- and that `lfs_ctz_extend()` then
uses in order instead of allocating and erasing. The allocator sees a
block as used only if a file reaches it (`lfs_fs_traverse_()`), so the open
file's reserved list must be reported there too; a power cut frees them, as
nothing committed points to them. RAM: one word a block, 2 kB for 2 MB.
With both patches a flight writes no data erase at all. The directory's
commits are appends already: littlefs 2.11 tracks the erased state of a
metadata block (its FCRC), so they erase only when a metadata block fills
and compacts -- every few hundred syncs, less with the log in a directory of
its own.

**The cost is proving them.** littlefs is the boards' filesystem: its
configuration, its pages, its identity. The patches must pass littlefs's
own test suite with its power-loss simulation, and new power-loss tests for
an append in place and for reserved blocks, before a board runs them. And we
carry a fork of littlefs until upstream has an equivalent.

| | Raw area | Patched littlefs |
|---|---|---|
| Flash layout | changes (D1): a reformat or a move of every file | unchanged |
| Flight log | a second storage path, rendered from the area | the same file and API |
| Data erases in flight | none | patch 1: one per 4 kB of log; patch 2: none |
| Code risk | ours alone, small | a filesystem fork; power-loss proof needed |
| Needs the chip to allow partial page programs | yes | yes |

Patch 1 alone takes most of the gain for the least change: one erase in
three minutes at the default logging rate. It is the one to prove first.

## What it would change

| | Today, littlefs | Pre-erased area |
|---|---|---|
| Per second at one row a second | one 4 kB erase, up to 4 kB copied, the metadata committed | one partial-page program |
| Longest stop of the flight core per write | 45 ms typical, 400 ms at most (a sector erase) | 0.4 ms typical, 3 ms at most (a page program) |
| Measured | by 4 minutes into the first 30 km bench flight, counted since boot: 238 erases, 762 programs, 609 conversions discarded (DD-068) | -- |

Times: W25Q128JV (MK1A), PDF page 66. Partial page programming: page 37 --
"less than 256 bytes (a partial page) can be programmed without having any
effect on other bytes within the same page". The RP2040 SDK programs whole
256-byte pages, so a partial program is a page of 0xFF around the new bytes:
0xFF leaves a programmed bit as it is.

MK1C's XT25F128F is not in `docs/datasheets`; its program and erase times,
and whether it allows partial page programs, must be read from its datasheet
before MK1C uses this.

Boards with an SD card do not need it: FatFs appends in place and the card
erases on its own time.

## Decisions

### D1. Where the area comes from

The flash maps today (CMakeLists.txt, the pfb linker script): a 36 kB
bootloader, 4 kB of OTA state, two application slots, then littlefs at the
end.

| Board | Flash | Slots | littlefs | Image today |
|---|---|---|---|---|
| MK1A, MK1C | 16 MB | 2 x 3948 kB | 8 MB | about 400 kB |
| MK1B | 2 MB | 2 x 384 kB | 984 kB | about 392 kB |

- **A. Shrink littlefs (8 MB to 6 MB) and put the area after it.** The OTA
  slots do not move, so an OTA update installs it. littlefs's geometry
  changes, so the first boot must move its files -- the configuration, the
  pins, the beeps, the web pages; all small -- into a newly formatted, smaller
  filesystem, or lose them. Recommended for MK1A and MK1C.
- **B. Take the top of the slots,** which hold a 400 kB image in 3.9 MB.
  Changing the slots moves the bootloader's layout: every board needs a
  BOOTSEL flash, and pico_fota_bootloader's swap would have to be read for
  what it copies and erases.
- **C. MK1B:** its whole flash is 2 MB. At most about 512 kB, from littlefs
  (984 kB to about 470 kB, if the web pages fit), which holds 6 hours at a
  row a second or 8 minutes at every sample.

### D2. When to erase

2 MB is 512 sectors: 23 s of erase stops at 45 ms each, and up to 205 s at
400 ms. Every one stops the flight core, as today's erases do.

- **A. At boot and after landing, a sector at a time on the storage task,
  never in flight.** Paced -- say one every 200 ms -- a full area takes about
  2 minutes, most of it while the board is on the bench. A launch before the
  area is ready flies into what is erased; the rest waits for landing.
  Recommended.
- **B. 64 kB blocks:** 32 erases of 150 ms (2 s at most). Fewer stops, each
  long enough to delay a launch detection by up to 2 s on the pad.
- **C. Erase on the bench only** (USB attached), refusing to arm without an
  erased area: nothing erases on the pad, but a board powered up on the pad
  with a used area has no log.

### D3. How often to program

- **A. Each second, a partial page:** the loss at a power cut stays one
  second, as FLT-LOG-06 asks; one program a second. Recommended, once the
  XT25F128F allows it.
- **B. Whole pages only:** at a row a second a page fills in 12 s, which a
  power cut can take; at every sample, four pages a second.
- **C. With either, time the program into the MS5607's idle stretch** so
  DD-068 discards nothing: the plan-2 option DD-073 set aside for
  discarding. More work: the storage task would wait for the sensor's cycle.

### D4. One flight or two

C6 decided to keep two flights. Two 1 MB halves, alternating: each holds
13 hours at a row a second, 16 minutes at every sample (22-byte records,
`flight_log.h`). Or one 2 MB area for the newest flight only.

### D5. How it is read

- **A. Rendered straight from the area:** `/api/flight.csv` reads it through
  the flight log's existing renderer; nothing is copied, and nothing in
  littlefs is written after landing. Recommended.
- **B. Copied into a littlefs file after landing:** the file API stays as it
  is; the copy costs 2 MB of littlefs writes on the ground.

## The format

The flight log's records (`flight_log.h`), each framed with a length and a
CRC as the high-rate log's are (DD-077), after a header naming the flight.
The end is the first record that is erased (0xFF) or fails its CRC, so a
power cut costs at most the record being programmed, and a boot finds where
a log stopped without any metadata.

## What it does not change

DD-068 still discards a conversion a program ran beside, unless D3-C:
about one a second, against two to three a second today. The pad marker stays
in littlefs, written on the pad.
