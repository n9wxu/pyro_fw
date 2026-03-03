# Petit FatFs Review for RP2040 Internal Flash

## Status: REVIEWED - Not recommended. Too restricted for our use case.

## What It Is
Stripped-down FatFs for extremely memory-constrained systems (8-bit MCUs).
Single volume, single file, no file creation, restricted writes.
Source: https://elm-chan.org/fsw/ff/00index_p.html

## Footprint on Cortex-M0 (RP2040)

| Feature | Code |
|---------|------|
| Base (mount + open) | 1,114B |
| + Read | +196B |
| + Write | +276B |
| + Directory | +370B |
| + Seek | +172B |
| **All features** | **~1,932B** |

RAM: 44 bytes work area. No sector buffer.

## Critical Write Restrictions
1. Cannot create files - only existing files
2. Cannot expand files - no append
3. Sector boundary writes only
4. No FAT table updates - never allocates clusters
5. No timestamp updates

## For Our Use Case

**Reading config.ini**: Perfect. 44 bytes RAM, ~1.3KB code.

**Writing telemetry CSV**: Cannot create or expand files.
Workaround: pre-create fixed-size file from USB, overwrite in place.
Too restrictive for flight_NNNN.csv naming.

**USB MSC**: Not applicable. USB needs direct sector access.

## Verdict
Too restricted. We need file creation (CSV) and append (telemetry).
Would work as read-only config parser but doesn't justify two FAT access paths.

Full FatFs adds ~8KB code but provides complete file access.
On RP2040 (264KB RAM, 2MB flash) the extra size is negligible.

## Recommendation
Use full FatFs instead. See docs/fatfs_review.md.
