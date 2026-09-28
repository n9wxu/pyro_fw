# Flight-log storage: the options

**Status: design, 2026-09-28. Nothing here is implemented.**

You adopted R1 (the flight path in RAM) and asked for an architecture that
also supports a second memory for logging, "so current boards operate and
future boards operate with SD cards": Petit FatFs on SD cards, littlefs on
internal flash, and RAM execution where there is a conflict with logging.
You then asked for the options to be listed against five hardware changes:

1. MK1C with an SD card.
2. MK1C with the RP2040 replaced by an ESP32-S3.
3. Option 2 with an SD card.
4. MK1C with the RP2040 replaced by an RP2350, and a second QSPI flash.
5. Option 4 with a QSPI RAM in place of the second flash, as a RAM disk
   and/or giant buffers.

Figures are cited to `docs/datasheets/` (file and page) or to a named
external document. Estimates are marked as such.

---

## 1. The problem

Today the flight log is a littlefs file on the flash the code executes from.
The audit in `docs/outstanding_tasks.md` section 10 found:

- **L2.** Each 1 s sync makes the next write copy the last partial block
  into a freshly erased one (`lfs_ctz_extend`), so the log erases flash
  about once a second in flight. On the RP2040 an erase or program disconnects
  execute-in-place for its whole duration, so core0 runs it with interrupts
  off: 46-73 ms measured (DD-035), and the part's worst case is hundreds.
- **L1.** Nothing bounds how many erases one littlefs call makes, the
  watchdog is 1000 ms, and a watchdog reset in flight comes back cold, which
  never deploys.
- **DD-068.** A flash write also disturbs a pressure conversion running
  beside it, electrically.
- **L3, L4, C6.** The log is truncated at launch and again by a brownout
  recovery; write failures go uncounted; the two kept flights you decided on
  are not implemented.

What every option must deliver:

| # | Goal |
|---|---|
| G1 | No log write delays the flight loop by more than a small bound (a millisecond or two), and none can stall it. |
| G2 | No lockup in flight: every wait bounded, proven where a proof exists (`support/prove_core0.py`). |
| G3 | MK1A, MK1B and MK1C keep working, on the hardware they have. |
| G4 | Future boards can log to an SD card. |
| G5 | Two flights kept; the log opened, and its header written, before OK to LAUNCH sounds (C6); a recovery appends to its flight's log (L3); every write failure counted (L4). |

---

## 2. The idea: a log medium's conflict class

What matters about a log medium is what a write to it does to the code
that is flying. There are three classes.

| Class | A write to it... | Examples | What the flight path needs |
|---|---|---|---|
| **FULL** | takes the memory bus the code executes from, for the whole erase or program | RP2040 internal flash; ESP32-S3 internal flash under ESP-IDF's default | R1: the flight path in RAM, the write on the other core |
| **BRIEF** | takes that bus only to send a command or read a status, microseconds; the erase itself runs inside another chip | RP2350 second flash on the QMI's chip select 1 | a small RAM routine for the command and poll, run only while the other core is out of execute-in-place; the flight path may stay in flash |
| **NONE** | uses a bus of its own | an SD card on SPI or SD/MMC; a NOR flash on an SPI peripheral; PSRAM holding the log in flight | nothing: the writer runs beside the flight path |

The evidence for each class:

- **RP2040:** an erase or program runs with execute-in-place disconnected
  for its duration. That is why core0 holds interrupts off across it today
  (DD-051, `littlefs_driver.c`).
- **RP2350:** in the QMI's direct mode the execute-in-place window is
  disconnected, and "attempting to access it generates a bus fault"
  (`rp2350-datasheet_2025-07-29.pdf`, section 12.14.5, page 1235). The
  bootrom's erase and program stay in direct mode for their whole duration
  (pages 387-388), so a second chip is only BRIEF with a driver of our own
  that leaves direct mode between the command and the status polls. A bus
  fault, not a stall, means even a microsecond window needs both cores out
  of execute-in-place.
- **ESP32-S3:** flash and PSRAM share the SPI0/1 interface
  (`esp32-s3_datasheet_v2.2.pdf`, page 20), and both cores share the caches
  (page 39). Under ESP-IDF's default a flash write disables the caches, parks
  the other core in a busy loop and runs only IRAM-safe interrupts (ESP-IDF
  v6.1, "Concurrency Constraints for Flash on SPI0/1"). Two escapes are
  documented: executing from PSRAM, and flash auto-suspend on flash chips
  that support it. The in-package flash does not by default (datasheet page
  13). The ESP-IDF page also states that flash on other SPI buses has no such
  constraint.

---

## 3. The software architecture, common to every option

### 3.1 Layers

```
 flight core                         writer                      medium
 ───────────                         ──────                      ──────
 hal_log_sample ─► log plan ─► record ring ─► log writer ─► log store ─► driver
 (DD-064)          (SPSC, RAM,      (core1 unit, or a      (one per      (flash, SD,
                    or PSRAM)        FreeRTOS task on the   medium)       NOR, PSRAM)
                                     ESP32-S3)
```

- **The record ring** replaces today's `memmove` buffer. It has one
  producer (the flight core) and one consumer (the writer), so neither
  waits on the other. A full ring drops records and counts them, as
  `log_dropped` does today.
- **The log store** is a table of operations, one per medium. It is named
  `*_vt` so `prove_core0.py` can follow it. Its operations:
  - *prepare*, on the pad: rotate the two kept flights, open, write the
    header, and pre-erase or pre-allocate. OK to LAUNCH waits for it (C6).
  - *append*, and *commit* (make it durable).
  - *resume*, after a reset in flight: append to the same flight's log.
  - *close*.
  - *read*, for the download, and *space*.
  - its conflict class, and counters for writes, failures and the longest
    operation.
- **The writer** drains the ring into the store. Its placement follows the
  store's class (3.2).

### 3.2 The executor policy, by class

- **FULL: R1, as analysed** (section 10, R1).
  - From launch to landing core0 runs only RAM code. About 38 KB of code and
    2 KB of constants per board; 50.6 KB of SRAM is free, 7-10 KB left once
    printf leaves the flight path.
  - Core1 does the store's operations as units core0 hands it; Lua pauses
    meanwhile.
  - The USB and alarm-pool interrupts are masked while a unit is out, an MPU
    guard faults any stray flash access, and `prove_core0.py` proves the
    flight closure RAM-closed.
  - About 13-19 engineer-days (the analysis's estimate).
- **BRIEF:** the writer issues each command and status poll from a RAM
  routine a few microseconds long, only while core1 idles in RAM. Today's
  flash window, shortened from tens of milliseconds to microseconds. The
  flight path stays in flash.
- **NONE:** the writer is a core1 unit (or, where it can be non-blocking, a
  core0 state machine polled once a loop), with no handshake. The flight path
  stays in flash.

**Placement is decided at build time.** A board declares its log media, a
primary and optionally a fallback. The flight path goes into RAM, and is
proved there, if any declared medium is FULL. An SD board that falls back to
internal flash when the card is missing therefore needs R1 too (3.4).

### 3.3 The backends

| Backend | Medium | Class | Notes |
|---|---|---|---|
| LFS-INT | littlefs on the code flash | FULL | Today's. Erases in flight (L2). |
| RAW-INT | a raw region of the code flash, pre-erased on the pad, built into a littlefs file after landing | FULL, programs only | Your earlier working-log idea (C6). In flight only page programs, with no erases. Needs a flash-layout change: the region comes out of the filesystem or the OTA slots. |
| PFF-SD | Petit FatFs on an SD card | NONE | Pre-sized files made on a PC (4.1). |
| FATFS-SD | FatFs on an SD card | NONE | `f_expand` pre-allocates on the pad, so no FAT write in flight. |
| RAW-SD | raw sectors on an SD card | NONE | Smallest, not readable by a PC without a tool; the board serves the CSV anyway. |
| NOR2 | littlefs or raw on a second NOR flash | NONE on an SPI peripheral; BRIEF on the RP2350's QMI chip select 1 | Soldered, so nothing to eject under load. |
| PSRAM | the log held in PSRAM in flight, persisted after landing | NONE in flight | Durability trade-off (option 5). |

### 3.4 What is shared whatever the medium

- **Two flights, prepared before OK to LAUNCH (C6).** A recovery resumes its
  own flight's log instead of truncating it (L3). A write that fails or
  half-succeeds is counted and its record realigned (L4).
- **The ground filesystem stays where it is.** Config, web files, pins,
  scripts and the pad marker stay in littlefs on internal flash on every
  option, locked in flight as today (DD-058).
- **DD-068 is generalised.** Today `flash_op_seq` advances on every internal
  flash operation. It becomes a storage-operation count that every medium's
  writes advance. An SD card draws more current on a write than the flash
  does: tens of milliamps in bursts, typically (an estimate; confirm against
  the chosen card or SD NAND part).
- **Read-back.** The HTTP CSV comes from whichever store holds the flight.
  An SD board could also present the card as a USB drive beside the network
  (a composite USB device); `docs/pure_fat12_remap_proposal.md` is prior art
  for a USB drive.
- **No sleeps (DD-053).** SD initialisation, card detection and status polls
  are loop steps with deadlines. The card's busy time after a write is
  hundreds of milliseconds at worst (the SD specification's write timeout;
  to confirm from the SD Association's simplified specification). It is
  polled, never waited on.
- **No card at boot.** Two choices, yours: fall back to internal flash, which
  costs R1 on SD boards too; or treat it as a pad fault and never sound OK to
  LAUNCH.

### 3.5 Current boards

MK1A, MK1B and MK1C keep their hardware. They get the common layer, R1, and
LFS-INT or RAW-INT. RAW-INT removes the erases from flight but needs the
flash-layout change. MK1B has 2 MB of flash, so its working-log size was
open (C6).

---

## 4. FAT on an SD card: the library

| | Petit FatFs R0.03a | FatFs R0.16 | Raw sectors |
|---|---|---|---|
| Code | 2-4 KB | Cortex-M3/Thumb-2: 6.1 KB default, 4.2 KB minimised (read/write) | under 1 KB (estimate) |
| RAM | 44 bytes of work area | 564 bytes a volume, and 552 a file (40 with `FF_FS_TINY`) | a 512-byte sector buffer |
| Create or grow a file | no | yes; `f_expand` pre-allocates contiguously | not applicable |
| Writes | whole sectors, from a sector boundary; one file | any | whole sectors |
| FAT metadata written in flight | never (the file's size is fixed) | none if pre-allocated before launch | none |
| Card preparation | pre-sized files, made on a PC | any FAT card | a tool to read it back |
| PC can read the card | yes | yes | no |

Sources: ChaN's pages, fetched 2026-09-28. For Petit FatFs, elm-chan.org's
`fsw/ff/00index_p.html` and `pf/write.html` ("Cannot create file... Cannot
append data and expand file size. Write operation can start/stop on the
sector boundary only"; finalising zero-fills the rest of the sector). For
FatFs, `fsw/ff/doc/appnote.html`, "Memory Usage".

**Petit FatFs and the one-second commit.** A commit finalises the current
sector, which zero-fills its tail. The next commit seeks back to that
sector's start and rewrites it with the new data. The file's size never
changes, so no FAT or directory sector is ever written. That makes it the
most robust choice against a power cut in flight, at the price of cards
prepared on a PC.

---

## 5. The hardware options

### Option 1: MK1C with an SD card

- **Hardware.** SPI0 on GPIO2-5, which MK1C leaves free: SCK, TX, RX and
  CSn (`rp2040-datasheet_2025-02-20.pdf`, section 2.19.2, Table 279).
  Card-detect on a free GPIO (9, 10 or 13-15).
  - Give the card its own supply filtering, away from the MS5607's supply.
    DD-068 found MK1B's sensor disturbed by its buzzer and by flash writes;
    MK1C's buzzer disturbed nothing, though both switch theirs with a
    MOSFET, so the difference is in the layout or supply (task B-BZ).
  - **1a: a microSD socket.** Choose a retention (hinged or push-pull, not
    push-push) that holds under boost and ejection loads.
  - **1b: a soldered SD NAND part.** Same protocol and FAT, nothing to
    eject, not removable.
  - **1c (not asked): a SPI NOR flash on the same pins.** Class NONE, no
    FAT, the smallest board change.
- **Class.** NONE for the SD. With a fallback to internal flash, R1 is still
  needed.
- **Software.** The common layer; an SD-over-SPI driver as loop steps
  (initialisation, card detect, block write with a polled busy); PFF-SD or
  FATFS-SD; the DD-068 count across SD writes. Estimate: 5-8 engineer-days
  beyond the common layer, and R1 only if a fallback is wanted.
- **Risks.** Card retention under load; the card's write current near the
  sensor; card quality and variability.

### Option 2: MK1C with an ESP32-S3

- **Hardware.** Two LX7 cores at 240 MHz, a single-precision FPU, 512 KB of
  SRAM (`esp32-s3_datasheet_v2.2.pdf`, pages 3-4). Optional in-package flash
  and PSRAM. Octal-PSRAM variants are rated to 65 C ambient; quad variants to
  85 or 105 C (page 13).
- **Class.** FULL for the internal flash. Under ESP-IDF's default a write
  parks the other core and disables the caches (section 2), so R1's "core1
  writes while core0 flies" does not map. The flight path would have to run
  in IRAM-safe interrupts during writes, or use flash auto-suspend (needs a
  flash chip that supports it; the in-package flash does not by default), or
  execute from PSRAM (which ESP-IDF documents as keeping the caches enabled).
- **Software: a port, not a change.**
  - A new HAL under `hal.h` on ESP-IDF and FreeRTOS. Lua becomes a task.
  - MK1C's arm pump moves from PIO, which the ESP32-S3 lacks, to RMT or
    MCPWM. The MS5607 one-shot moves to a timer interrupt in IRAM.
  - The ADC needs recalibrating for pyro sense.
  - The USB network goes through ESP-IDF's TinyUSB (whether ECM is supported
    is to confirm), and OTA through ESP-IDF's partitions in place of
    `pico_fota_bootloader`.
  - `prove_core0.py` is ARM-specific; an Xtensa equivalent is needed.
  - Rough estimate: 40-80 engineer-days, most of it outside logging.
- **Gains.** More RAM and speed, an FPU, Wi-Fi and BLE (a UI without a
  cable), PSRAM.
- **Risks.** The size of the port; radio emissions near pyrotechnics; power
  draw on the pad; ADC quality for sense.

### Option 3: Option 2 with an SD card

- **Hardware.** The SD/MMC host controller, 1- or 4-bit, two slots
  (`esp32-s3_datasheet_v2.2.pdf`, page 4). Its own peripheral, off SPI0/1.
- **Class.** NONE. FATFS-SD, from ESP-IDF's FatFs and SD/MMC driver, run by a
  writer task on the other core. No IRAM flight path is needed for logging,
  unless the fallback is internal flash.
- **Software.** Option 2's port, and 3-5 engineer-days (estimate).
- **Risks.** Option 2's, and option 1's card risks.

### Option 4: MK1C with an RP2350 and a second QSPI flash

- **Hardware.**
  - 520 kB of SRAM in 10 banks, and a second 16 MB chip select
    (`rp2350-datasheet_2025-07-29.pdf`, page 13).
  - Cortex-M33 cores with FPU extensions (page 35).
  - QMI CS1n on GPIO0, 8, 19 or 47 (page 21). It needs an external pull-up
    so the chip does not power up selected (page 593).
  - Bootrom A/B booting (section 5.10.4), which could replace
    `pico_fota_bootloader`.
  - Erratum RP2350-E9 on the A2 stepping (page 1366): a Bank 0 input between
    logic levels leaks about 120 uA and holds itself near 2.2 V, and a
    pull-down cannot overcome it. Check MK1C's high-impedance sense and bias
    nodes against it, or use a later stepping.
- **4a: the second flash on QMI chip select 1.** Class BRIEF, with a driver
  of our own (section 2). The bootrom's calls hold direct mode for the whole
  erase. The flight path can stay in flash. Reads of the log are
  memory-mapped.
- **4b: the second flash on SPI0 or SPI1.** Class NONE, with no QMI
  involvement at all. Slower, but the log needs about 1.1 KB/s at full rate.
- **Software.**
  - Porting RP2040 to RP2350 with the same SDK (pico-sdk 2.x builds either)
    is mostly a rebuild. The register-level code (`ms5607_bus.h`, timers) and
    the bootloader need checking, and `prove_core0.py` needs checking against
    Cortex-M33 code.
  - Rough estimate: 8-15 engineer-days for the port, 3-6 for the NOR2
    backend.
  - With twice the SRAM, R1 itself becomes easy, if it is still wanted for
    the fallback.
- **Risks.** E9 on the stepping bought; the second chip-select's pull-up and
  routing.

### Option 5: Option 4 with a QSPI PSRAM on chip select 1

- **Hardware.** A PSRAM on QMI chip select 1, up to 16 MB, memory-mapped for
  reads and writes once XIP_CTRL.WRITABLE_M1 is set
  (`rp2350-datasheet_2025-07-29.pdf`, page 347). The QMI enforces the
  PSRAM's maximum chip-select time for its refresh (MAX_SELECT, page 1227).
  A part such as an 8 MB APS6404L is typical; its figures are to be taken
  from its datasheet.
- **Class in flight.** NONE: nothing is written to flash between launch and
  landing, so there is nothing to conflict with. Note that on the ground a
  flash write puts the QMI in direct mode, so the PSRAM is unreachable too
  (page 1235). Nothing that touches PSRAM may run during a ground flash
  write.
- **Uses of the PSRAM:**
  - **The whole flight's log.** At full rate the log is about 1.1 KB/s
    (50 rows a second of 22 bytes), so 8 MB holds about two hours. It is
    persisted to internal flash after landing: no flash write in flight, and
    no R1.
  - **A RAM disk.** littlefs or FAT over a PSRAM block device for anything
    written in flight, copied to flash after landing.
  - **Giant buffers.**
    - A pre-launch ring of minutes at full rate, which makes the events
      plan's one-second windows (DD-064) unnecessary.
    - lwIP's heap (task N1, which today is out of room under load at 8 KB).
    - The Lua heap, HTTP buffers, and OTA staging.
- **The trade-off: durability.** PSRAM is volatile.
  - A power cut in flight loses whatever was not yet persisted. Today's
    once-a-second commit (FLT-LOG-06) exists for exactly that case.
  - A watchdog reset leaves the PSRAM powered, so the log may survive for a
    recovery to resume. Whether our PSRAM initialisation sequence preserves
    the contents is to be verified against the part.
  - **Hybrid:** PSRAM for everything, and a low-rate commit of it to a
    NONE-class medium (option 4b's NOR, or an SD card).
- **Software.** Option 4's port, and a PSRAM ring-and-persist backend, 4-7
  engineer-days (estimate).
- **Risks.** Durability as above; PSRAM availability and temperature rating.

---

## 6. Comparison

Engineer-day figures are rough estimates, for comparison only. Each includes
the common layer's 5-8 days where the option needs it.

| Option | MCU | Log medium | Class in flight | R1 needed | Software (estimate) | Survives a power cut in flight | Main risk |
|---|---|---|---|---|---|---|---|
| Today's boards | RP2040 | internal flash (LFS-INT or RAW-INT) | FULL | yes | 18-27 days | yes, to the last commit | the RAM budget, 7-10 KB spare |
| 1 | RP2040 | SD (or NOR on SPI0, 1c) | NONE | only for a fallback | 10-16 days, plus R1 for a fallback | yes, to the last commit | card retention, write current |
| 2 | ESP32-S3 | internal flash | FULL | an equivalent in IRAM, or auto-suspend | 40-80 days | yes | the size of the port |
| 3 | ESP32-S3 | SD over SD/MMC | NONE | no | option 2 and 3-5 days | yes | the port, and card retention |
| 4a | RP2350 | second flash on QMI chip select 1 | BRIEF | no | 16-29 days | yes | a custom QMI driver; E9 |
| 4b | RP2350 | second flash on SPI | NONE | no | 16-29 days | yes | E9 |
| 5 | RP2350 | PSRAM, persisted after landing | NONE | no | 17-30 days | only to the last persist | durability |

---

## 7. Decisions for you

1. **The FAT library for SD:** Petit FatFs with pre-sized files, FatFs with
   `f_expand`, or raw sectors (section 4).
2. **No usable card at boot:** fall back to internal flash (R1 on SD boards
   too), or a pad fault.
3. **Option 5's durability:** PSRAM alone, or PSRAM with a low-rate commit
   to a second, NONE-class medium.
4. **Which options to pursue,** and in what order. Today's boards need the
   common layer and R1 whichever is chosen (G3).
5. **Option 2's radio:** off in flight, off altogether, or used for
   telemetry.

---

## 8. Sources

- `docs/datasheets/rp2040-datasheet_2025-02-20.pdf`: section 2.19.2,
  Table 279.
- `docs/datasheets/rp2350-datasheet_2025-07-29.pdf`: pages 13, 21, 35, 347,
  387-388, 593, 1227, 1235, 1366; section 5.10.4.
- `docs/datasheets/esp32-s3_datasheet_v2.2.pdf`: pages 3, 4, 13, 20, 39.
- ESP-IDF Programming Guide v6.1, "Concurrency Constraints for Flash on
  SPI0/1", docs.espressif.com, fetched 2026-09-28.
- ChaN, Petit FatFs (`elm-chan.org/fsw/ff/00index_p.html`,
  `fsw/ff/pf/write.html`) and FatFs application note
  (`fsw/ff/doc/appnote.html`), fetched 2026-09-28.
- In this tree: `docs/outstanding_tasks.md` section 10 (L1-L6, R1, N1);
  DD-035, DD-051, DD-053, DD-058, DD-064, DD-068;
  `docs/pure_fat12_remap_proposal.md`.
- To be added before a board is designed: the chosen flash, SD or SD NAND,
  and PSRAM parts' datasheets, and the SD Association's simplified physical
  layer specification for the card's write timeout.
