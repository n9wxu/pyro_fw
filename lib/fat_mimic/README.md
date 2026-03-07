# fat_mimic

FAT12 USB Mass Storage over littlefs for RP2040.

## Overview

Presents a writable FAT12 filesystem to USB hosts (macOS, Windows, Linux) while persisting all changes to littlefs on flash. Designed for embedded devices that need user-editable configuration files via USB.

## Architecture

Three-layer design:

- **USB layer** (callbacks): RAM-only operations. No flash I/O in USB callbacks prevents XIP/DMA conflicts on RP2040.
- **Sync layer** (main loop poll): Flushes dirty data to littlefs one file per call. Waits for USB bus quiet period before touching flash.
- **Boot layer** (init): Builds FAT12 image from littlefs contents on USB mount.

## RAM Usage (~35KB)

| Component | Size | Purpose |
|-----------|------|---------|
| FAT table cache | 2 KB | OS reads/writes FAT allocation table |
| Root directory cache | 16 KB | OS reads/writes directory entries |
| Dirty sector cache | 16 KB | Write-back cache for data sectors |
| File table | 512 B | Cluster → littlefs path mapping |

## Sync Triggers

Three conditions trigger a flush from dirty cache to littlefs:

1. **Last root directory sector written** — full batch complete (covers create, duplicate, rename)
2. **Directory slot reuse** (0xE5 → valid entry) — new file in previously deleted slot
3. **Gap detection** — non-directory write after directory write indicates partial update complete

File deletions from littlefs only occur on USB unmount/eject to prevent premature removal during multi-step OS operations.

## API

```c
#include "fat_mimic.h"

// Boot: mount littlefs
fat_mimic_init(&lfs_config);

// USB mount callback: build FAT image
fat_mimic_mount();

// USB read/write callbacks (called from TinyUSB MSC)
fat_mimic_read(lun, lba, buf, len);
fat_mimic_write(lun, lba, buf, len);

// Main loop: flush dirty data to flash
fat_mimic_poll();

// USB unmount/eject: flush + delete orphans
fat_mimic_unmount();
```

## Integration

### CMake (FetchContent)

```cmake
FetchContent_Declare(fat_mimic
    GIT_REPOSITORY https://github.com/your/fat_mimic
    GIT_TAG main
)
FetchContent_MakeAvailable(fat_mimic)
target_link_libraries(your_target fat_mimic)
```

### CMake (subdirectory)

```cmake
add_subdirectory(lib/fat_mimic)
target_link_libraries(your_target fat_mimic)
```

## Dependencies

- [littlefs](https://github.com/littlefs-project/littlefs) (linked as `littlefs_lib`)
- TinyUSB (via Pico SDK `tinyusb_device`)
- Pico SDK (`pico_stdlib`)

## Limitations

- FAT12 only (max ~680KB usable space)
- 16 files maximum
- 8.3 filenames (LFN entries preserved in cache but not decoded)
- Single root directory (no subdirectories)
- 32 dirty sector cache slots (larger writes use backpressure eviction)
- Flash writes blocked during active USB transfers (50ms quiet gate)

## License

MIT
