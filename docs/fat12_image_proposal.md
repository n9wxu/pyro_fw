# fat_mimic Alternative: FAT12 Image on littlefs

## Status: PROPOSAL - Not implemented. Saved for future comparison.

## Summary
Instead of translating between FAT12 and littlefs file semantics, store the entire FAT12 partition as a single littlefs file (`fat12.img`). The OS writes FAT12 sectors, we store them verbatim. littlefs handles wear leveling.

## Architecture
```
USB Host ←→ RAM cache (FAT + root dir) ←→ littlefs file "fat12.img" ←→ flash
```

## Advantages over current fat_mimic
- **Byte-perfect FAT12**: No translation, no edge cases
- **Less RAM**: ~26KB vs ~35KB (no dirty sector cache)
- **Less code**: ~200 lines vs ~400 lines
- **LFN works**: Long filenames stored in image, survive reboot
- **Renames work**: No name translation issues
- **Temp files handled**: FAT12 manages them natively

## Eliminates
- File table / cluster_map
- Dirty sector write-back cache (16KB)
- FAT chain walking
- Name translation (8.3 ↔ littlefs)
- Orphan detection/deletion
- Complex sync triggers

## Keeps
- RAM cache for FAT + root dir (fast reads)
- Dirty bitmap (which sectors need flushing)
- 50ms USB quiet gate
- One sector per poll flush

## Trade-offs
- Data reads hit flash (XIP, safe but slower than RAM)
- Write amplification: 8x (512B sector → 4096B littlefs block)
- Flash life estimate: ~100 years at 10 edits/day
- Firmware reading config.ini requires parsing FAT12 image (or keeping a separate littlefs copy synced on unmount)
- Partition size: ~660KB image + littlefs overhead needs ~750KB flash

## RAM Budget
| Component | Size |
|-----------|------|
| FAT cache | 2KB |
| Root dir cache | 16KB |
| Dirty bitmap | 168B |
| littlefs buffers | ~8KB |
| **Total** | **~26KB** |
