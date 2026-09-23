#!/bin/bash
# Copy the board plant models into a QEMU tree that has the pyro-plant device.
#
# The plant lives in THIS repository. The copy under hw/misc/pyro-plant/ in
# the QEMU tree is generated and must not be edited there: QEMU's meson
# cannot reach outside its own source root, so the files have to be present
# inside it.
#
#   ./sim/qemu/sync-plant.sh ~/src/qemu-rp2040-pico
#
# Run it after changing anything in sim/plant/, then rebuild QEMU.
set -e
QEMU="${1:?usage: sync-plant.sh <path-to-qemu-tree>}"
SRC="$(cd "$(dirname "$0")/../plant" && pwd)"
DST="$QEMU/hw/misc/pyro-plant"
mkdir -p "$DST"
for f in plant.h plant_internal.h net_solve.h net_solve.c plant.c \
         plant_mk1a.c plant_mk1b.c plant_mk1c.c; do
    cp "$SRC/$f" "$DST/$f"
done

# Each plant_mk1*.c includes its own board's board_pins.h, and the three
# headers define the same macros with different values -- so each gets its
# own include directory and its own static library on the QEMU side.
for b in mk1a mk1b mk1c; do
    mkdir -p "$DST/$b"
    cp "$SRC/../../boards/$b/board_pins.h" "$DST/$b/board_pins.h"
done
echo "synced plant + 3 board pin maps into $DST"
