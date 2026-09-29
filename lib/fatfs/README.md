# FatFs R0.16

ChaN's FatFs, from elm-chan.org/fsw/ff/arc/ff16.zip (released 2025-07-22),
fetched 2026-09-29, with elm-chan.org/fsw/ff/patch/ff16p1.diff and
ff16p2.diff applied to `ff.c` (the published fixes, CVE-2026-6682..6688).
Line endings converted to LF; otherwise unmodified.

The configuration (`ffconf.h`), the disk layer (`diskio.c`) and the OS layer
(`ffsystem.c`) are this firmware's: `src/sd/`.

License: `LICENSE.txt` (FatFs license, BSD-style, one clause).
