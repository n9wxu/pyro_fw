# Datasheets

The primary sources for every figure the firmware and its documents take
from a part. Cite the file and page, not memory.

| File | Part | Revision | Source |
|---|---|---|---|
| `MS5607-02BA03_2017-06.pdf` | TE MS5607-02BA03 barometric pressure sensor (MK1B, MK1C) | 06/2017 | farnell.com/datasheets/2917207.pdf, fetched 2026-09-26; TE's later revisions would not download |

MS5607, page 3: conversion time at OSR 4096 is 7.40 / 8.22 / 9.04 ms (min /
typ / max). Page 4 (pressure output): RMS resolution 0.024 mbar (2.4 Pa) at
OSR 4096, 0.036 at 2048, 0.054 at 1024, 0.084 at 512, 0.130 at 256.
