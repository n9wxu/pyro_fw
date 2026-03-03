# Pure FAT12 on Flash with Cluster Remapping

## Status: PROPOSAL - Not implemented. Preferred alternative to fat_mimic.

## Summary
Real FAT12 filesystem directly on flash with wear leveling via cluster remapping. No littlefs dependency. 4096-byte clusters match flash erase blocks for zero write amplification. MBR and boot sector generated from compile-time constants, never stored.

## Architecture
```
USB Host ←→ RAM cache (FAT + root dir) ←→ remap table ←→ flash blocks
Firmware  ←→ local FAT12 API ──────────↗
```

## Geometry
4096-byte clusters (8 × 512-byte sectors). MBR + boot sector generated on read.

```
FAT12 Partition (presented to USB host):
  Sectors 0-7:   Reserved (boot sector + padding)  generated, not stored
  Sectors 8-15:  FAT table                          = logical cluster 0
  Sectors 16-23: Root directory (128 entries)        = logical cluster 1
  Sectors 24+:   Data area                           = logical clusters 2-161

  162 logical clusters, 1304 partition sectors
  MBR at sector 0, partition at sector 63
  Total disk: 1367 sectors (700KB)
```

## Flash Layout
```
Block 0-63:    Firmware (256KB reserved)
Block 64:      Remap table copy A
Block 65:      Remap table copy B
Block 66-447:  Cluster pool (382 physical blocks for 162 logical clusters)

Wear leveling ratio: 382 / 162 = 2.4x
```

## Cluster Remapping
```c
uint16_t remap[162];       // logical cluster → physical block (324 bytes)
uint8_t  free_bitmap[48];  // 382 bits, 1 = free
uint8_t  dirty_bitmap[21]; // 162 bits, 1 = needs flush
uint32_t remap_seq;        // sequence number for A/B power-loss safety
```

Write: allocate new physical block → write 4096 bytes → update remap → free old block
Read: remap[logical_cluster] → read physical block via XIP

## RAM Budget
| Component | Size | Notes |
|-----------|------|-------|
| FAT cache | 4KB | Logical cluster 0, served from RAM |
| Root dir cache | 4KB | Logical cluster 1, served from RAM |
| Cluster write buffer | 4KB | Assembles partial 512B sector writes |
| Remap table | 324B | 162 × 2 bytes |
| Free bitmap | 48B | 382 blocks |
| Dirty bitmap | 21B | 162 clusters |
| **Total** | **~12.5KB** | |

## USB Interface

Read (callback, no flash writes):
- MBR, boot sector, reserved sectors → generated from constants
- FAT, root dir → RAM cache
- Data sectors → compute cluster, remap[cluster] → flash XIP read

Write (callback, RAM only):
- FAT, root dir → update RAM cache, mark dirty
- Data sectors → buffer in cluster write buffer, mark dirty

Poll (main loop, 50ms USB quiet gate):
- One dirty cluster per call: allocate new block, write, update remap, free old
- Persist remap table after all dirty clusters flushed

## Local Filesystem API
```c
int  fat_open(const char *name, uint8_t mode);
int  fat_read(int fd, void *buf, uint32_t len);
int  fat_write(int fd, const void *buf, uint32_t len);
int  fat_close(int fd);
int  fat_create(const char *name);
int  fat_remove(const char *name);
```
Same code path as USB: FAT + root dir from RAM, data via remap table.

## Power-Loss Safety
- Remap table: two copies (blocks 64-65), alternating writes, sequence numbered
- Data write: new block written before remap updated; old data intact if power lost
- On boot: read both remap copies, use higher sequence number
- Worst case: lose last write, filesystem consistent at previous state

## Wear Leveling
- 382 physical blocks for 162 logical clusters = 2.4x ratio
- 100,000 erase cycles × 2.4 = 234,000 effective writes per cluster
- FAT cluster (~10 writes per edit): 234,000 / 10 = 23,400 edit sessions
- At 10 edits/day: ~6.4 years. At 1 edit/day: ~64 years.

## Comparison
| Aspect | fat_mimic (current) | FAT12 image on lfs | Pure FAT12 + remap |
|--------|---------------------|--------------------|--------------------|
| RAM | 35KB | 26KB | **12.5KB** |
| Correctness | Translation bugs | Perfect FAT12 | Perfect FAT12 |
| Dependencies | littlefs | littlefs | **None** |
| Local file access | littlefs API | Parse image | **Native FAT12** |
| Flash overhead | lfs metadata | lfs metadata | **8KB (2 blocks)** |
| Write amplification | Via littlefs | 8x (lfs block) | **1x (aligned)** |
| MBR/boot sector | Stored | Stored | **Generated** |

## Advantages
1. No littlefs dependency - standalone, reusable library
2. Lowest RAM (12.5KB)
3. Perfect FAT12 - no translation, no edge cases
4. Native file access for firmware (config.ini, telemetry CSV)
5. Zero write amplification (cluster = flash block)
6. Minimal flash overhead (2 blocks for remap)
7. MBR/boot sector are compile-time constants, never corrupted

## Risks
1. Custom wear leveling needs thorough testing
2. Power-loss safety simpler than littlefs but must be correct
3. ~500 lines of code (more than image approach, less than current fat_mimic)
4. No subdirectory support (FAT12 root dir only)
