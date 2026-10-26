# A Pre-Erased Flight Log On The QSPI Flash

The flight log is `flight_log.bin`, a file on the board's flash store: alfs,
format 6, in the area at the top of flash, behind `src/vfs.h` (DD-100). A
flight only programs it. Every page the log takes was erased on the ground by
the cleanup, and from launch to landing nothing erases unless the log has no
erased page left (FLT-LOG-10).

The log keeps a row for every sample and every event row at its own time
(FLT-LOG-07, DD-101), and a flight that never lands loses at most its last
second (FLT-LOG-06).

On a board with an SD card mounted the log is a file on the card (DD-076),
and none of this applies: FatFs appends in place and the card erases on its
own time.

## Decisions

### D1. Where the area comes from

The log shares the flash store with the board's other files -- the
configuration, the pin and beep tables, the web pages, the script, the pad
marker. The store is the area pico_fota_bootloader leaves above its 36 kB
bootloader, 4 kB of OTA state and two application slots (`PYRO_PFB_FS_KB` in
each `boards/<name>/board.cmake`, `src/fs_flash.c`). The log's room is what
the other files leave, and `/api/log/space` reports it (WEB-API-12).

| Board | Flash | Store | Log, every sample (DD-101) |
|---|---|---|---|
| MK1A | W25Q128JV, 16 MB | 8 MB | about 1.5 hours |
| MK1C | XT25F128F, 16 MB | 8 MB | about 1.5 hours |
| MK1B-8MB | BY25Q64ES, 8 MB | 4 MB | about 39 minutes |
| MK1B | W25Q16JV, 2 MB | 984 kB | about 9 minutes |

### D2. When to erase

Only the cleanup erases (`vfs_clean()`): one sector a pass of the storage
task, and never while the flight log holds the filesystem (`clean_service()`
in `src/hal_common/hal_common.c`). On USB with test mode off it takes the next
sector at each pass; otherwise one a second (`vfs_flash_clean_wait_ms()`,
USB-10, DD-107). On a part whose erase suspends, the cleanup's erase runs in
the writer's slots in pieces, each stopping core0 for at most the 3 ms slot
(FLT-RT-06, DD-112). On BY25Q64ES (MK1B-8MB) an erase runs whole and stops
both cores (`flash_op()`, DD-074): 50 to 70 ms with no pressure reading,
launch detection and the ground reference held, and the periods it costs
counted as `loop_overruns_ground` (FLT-RT-05).

From launch to landing the writer erases only when the log has no erased
page left, and only on a part whose erase ends in suspended pieces: one
sector, suspended at the end of each of the writer's slots and resumed in
the next (FLT-LOG-10, `src/flash_slot.c`). BY25Q64ES's erase does not end in
such pieces, so MK1B-8MB does not erase in flight and its log has the room
erased before launch (`src/flash_timing.c`, DD-104). An erase or program the
chip holds suspended is run to its end at every start and on the ground
after a flight that gave one up (FLT-LOG-11).

### D3. How often to program

The flight task puts each record into a 4 kB ring in RAM and never waits.
The storage task, the writer of core1's frame (FLT-LOG-09), writes the ring
out every 200 ms and syncs the file every second (FLT-LOG-06). A sync stores
the partial page too, at the cost of the rest of that page (DD-100), so the
log takes about six pages a second: 5.9 measured on MK1C over a 177.5 s bench
flight (DD-101).

alfs programs only the bytes it means. Each program is one page program of
exactly those bytes, leaving the page's other bytes alone
(`flash_op_slot_program()`, `src/flash_op.h`). From the first period airborne
until landing it runs on core1 with core1's interrupts off, in the writer's
first 3 ms of the period, and starts only when by the chip's datasheet it
ends or can be suspended before the slot does (`src/flash_slot.h`);
BY25Q64ES's program, 2.4 ms at worst, starts only in the slot's first
300 us. Nothing on core0 stops for it (FLT-RT-03).

The log's file opens at the head of the first period airborne, and its rows
wait in the ring until then (DD-104). Its first write waits until the ring
fills or 2 s have passed since the file opened, so the launch shock passes
with the flash idle (DD-027). A conversion a program ran beside is used like any other and
counted, `pres_flashed` on `/api/status` (DD-106).

### D4. One flight or two

One. The log's open removes the last flight's log first, so a reset in
flight finds this flight's: a write replaces a file only when it closes
(DD-100). `POST /api/flight/erase` removes it on purpose.

### D5. How it is read

`GET /api/flight.csv` renders the file as CSV as it is read
(`flog_csv_read()`, `src/flight_log.c`), framed by Content-Length: the
server counts the rendered length first, then streams the same rendering
(DD-062). It is refused with 423 while the log holds the storage
(WEB-API-10). In test mode with `log=host` the log goes to RAM for the host
in place of the flash, read at `/api/flight/live` (USB-09, DD-103).

## The format

The records are in `src/flight_log.h` (DD-062). A file is the magic `PYL1`,
one header record, then sample and text records in the order they were
written; multi-byte fields are little-endian.

| Record | Type byte | Fields |
|---|---|---|
| Header | `H` | length, ID and name (8 bytes each), each channel's mode and value, units, ground pressure, the plan byte (always 2, every sample), the board's name |
| Sample | `S` | time, pressure, altitude, the raw reading, temperature in tenths of a degree, state, thrust, event: 22 bytes with the type |
| Text | `T` | time, tag (Lua, mock, estimator), length, up to 64 characters |

The CSV's header says `# Log rate: every sample`, which `pyro_sim --replay`
reads; it refuses a log whose header says it kept fewer (DAT-08).

The end is where the file's stored bytes end: a reader gets the log to its
last synced page (DD-105), and a record cut short there ends the log, with
everything before it rendered.
