# Filesystem Architecture Analysis

## Status: ANALYSIS COMPLETE - Decision pending.

## Options Evaluated

### 1. fat_mimic (current implementation)
- Location: `lib/fat_mimic/`
- Status: Working but has edge cases
- Approach: Virtual FAT12 over littlefs with translation layer
- RAM: ~35KB | Code: ~400 lines | Dependencies: littlefs
- Issues: Name translation bugs, sync timing, temp file accumulation
- Local access: Via littlefs API (separate from FAT12)

### 2. FAT12 image on littlefs
- Proposal: `docs/fat12_image_proposal.md`
- Approach: Store entire FAT12 partition as single littlefs file
- RAM: ~26KB | Code: ~200 lines | Dependencies: littlefs
- Pro: Perfect FAT12, simple USB path
- Con: Local file access requires parsing FAT12 image or separate littlefs copies

### 3. Pure FAT12 + cluster remap
- Proposal: `docs/pure_fat12_remap_proposal.md`
- Approach: Real FAT12 on flash, wear leveling via cluster remapping
- RAM: ~12.5KB | Code: ~500 lines | Dependencies: None
- Pro: Lowest RAM, no dependencies, native local API
- Con: Must implement FAT12 correctly, most code to write

### 4. FatFs + cluster remap (RECOMMENDED)
- Review: `docs/fatfs_review.md`
- Approach: ChaN's FatFs for local access, direct sectors for USB, shared remap layer
- RAM: ~14KB | Code: ~300 lines new + FatFs ~10KB | Dependencies: FatFs
- Pro: Proven FAT12 handling, full local file API, least risky new code
- Con: Slightly more RAM than pure custom, external dependency

### 5. Petit FatFs + remap
- Review: `docs/petit_fatfs_review.md`
- Status: REJECTED - Cannot create files or expand them
- Only useful as read-only config parser

## Key Findings

### FAT32 is impossible
Our volume (~700KB) is far below FAT32's minimum (~32MB).
FAT12 is the only option. No FAT variant has wear leveling.
Flash concerns must be solved in the disk I/O layer.

### Simultaneous access is impossible
USB MSC is block-level with no change notification mechanism.
Host caches sectors and assumes exclusive ownership.
Simultaneous MCU + host access = guaranteed corruption.
Solution: mutual exclusion. FatFs when USB unmounted, direct sectors when mounted.

### Wear leveling is essential
FAT table is a write hotspot (updated every file operation).
Without wear leveling: ~2.7 years to flash death at moderate use.
Cluster remap with 2.4x spare ratio: ~6-64 years depending on usage.

### 4096-byte clusters are optimal
Match RP2040 flash erase block size exactly.
Zero write amplification.
Trade-off: minimum 4KB per file (acceptable for ~10 files).

## Comparison Matrix

| Aspect | fat_mimic | Image+lfs | Custom+remap | FatFs+remap |
|--------|-----------|-----------|--------------|-------------|
| New code | 400 (done) | ~200 | ~500 | ~300 |
| Total code | 400 | 200+lfs | 500 | 300+10K |
| RAM | 35KB | 26KB | 12.5KB | 14KB |
| FAT12 correct | Bugs | Perfect | Must test | Proven |
| Local file API | littlefs | Complex | Custom | f_open etc. |
| Wear leveling | Via lfs | Via lfs | Custom remap | Custom remap |
| Dependencies | littlefs | littlefs | None | FatFs |
| Risk | Medium | Low | High | Low |
| LFN support | Cache only | Full | None | Built-in |

## Recommendation
FatFs + cluster remap layer. Least risky new code, proven FAT12 handling,
full local file API, clear mutual exclusion model.
