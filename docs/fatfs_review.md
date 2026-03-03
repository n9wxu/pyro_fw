# FatFs Review for RP2040 Internal Flash

## Status: REVIEW - Evaluating FatFs for use with cluster remap layer.

## What FatFs Is
ChaN's mature FAT filesystem library. ANSI C89, BSD-style license, 20+ years of development.
Handles FAT12/16/32, LFN, exFAT, thread safety. You implement 5 disk I/O functions.
Source: https://elm-chan.org/fsw/ff/

## Footprint (ARM Thumb-2, closest to RP2040 M0+)

| Config | Code | RAM/volume | RAM/file |
|--------|------|------------|----------|
| R/W all functions | 6.1KB | 564B | 552B |
| R/W minimal | 2.1KB | 564B | 552B |
| + LFN (SBCS) | +3.3KB | - | - |
| Tiny file objects | - | 564B | 40B |

With LFN + 2 open files: ~10KB code, ~1.7KB RAM.

## RP2040 Flash Constraints
1. 4096-byte sector erase required before programming
2. XIP disabled during erase/program (halts code + USB DMA)
3. No hardware wear leveling
4. Must disable interrupts during flash operations

## Critical Issue: FAT Table Hotspot
FAT table at fixed sectors, updated on every file operation.
Without wear leveling at 10 ops/session, 10 sessions/day:
100,000 erase cycles / 100 daily = 1,000 days ≈ 2.7 years to flash death.

## Recommended Architecture

```
USB Host ←→ RAM cache ←→ remap layer ←→ flash
Firmware ←→ FatFs ──────→ remap layer ──↗
```

FatFs and USB never run simultaneously:
- USB active when mounted (direct sector access via remap)
- FatFs active when unmounted (boot config read, post-flight CSV write)

## USB MSC Integration
USB callbacks bypass FatFs entirely:
- Writes → RAM cache + dirty bitmap (no flash in callback)
- Reads → RAM cache for FAT/dir, remap layer for data (XIP safe)
- Poll → flush dirty sectors through remap layer (50ms quiet gate)
- On USB unmount → f_mount to resync FatFs with changes

## Recommended FatFs Configuration
```c
#define FF_FS_READONLY   0
#define FF_FS_MINIMIZE   0
#define FF_USE_LFN       1     // Static buffer, no malloc
#define FF_MAX_LFN       64
#define FF_CODE_PAGE     437   // Smallest codepage
#define FF_FS_TINY       1     // 40 bytes/file vs 552
#define FF_USE_MKFS      1     // Format on first boot
#define FF_MIN_SS        512
#define FF_MAX_SS        4096  // Match flash erase block
#define FF_FS_LOCK       0
#define FF_FS_REENTRANT  0
#define FF_USE_TRIM      0
```

## Comparison: FatFs + Remap vs Custom FAT12

| Aspect | FatFs + remap | Custom FAT12 + remap |
|--------|---------------|---------------------|
| Code size | ~10KB + 300 lines | ~500 lines total |
| Correctness | Proven, 20+ years | Must test thoroughly |
| LFN | Built-in | Not included |
| Local file API | Full (f_open etc.) | Custom minimal |
| USB integration | Bypass for MSC | Direct sector access |
| RAM | ~14KB total | ~12.5KB total |
| Maintenance | Community | Self |

## Bottom Line
FatFs is excellent for local file access (config.ini, CSV). For USB MSC, we still
need RAM cache + remap because FatFs can't run in USB callbacks. Best approach:
share the remap layer between FatFs (firmware) and direct sector access (USB).
