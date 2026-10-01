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

# The rocket, too. The pyro-plant device flies it so the BMP280 model has a
# pressure to report and the firmware has a flight to fly, and it must be the
# same physics the host simulator uses or the two disagree about what a
# flight looks like.
for f in physics.h physics.c; do
    cp "$SRC/../$f" "$DST/$f"
done

# Each plant_mk1*.c includes its own board's board_pins.h, and the three
# headers define the same macros with different values -- so each gets its
# own include directory and its own static library on the QEMU side.
for b in mk1a mk1b mk1c; do
    mkdir -p "$DST/$b"
    # Whatever headers that board's plant model reaches for. board_pins.h is
    # always one; MK1C also shares the firmware's sense thresholds so the
    # model and the firmware cannot disagree about them.
    for h in board_pins.h pyro_sense.h; do
        [ -f "$SRC/../../boards/$b/$h" ] && cp "$SRC/../../boards/$b/$h" "$DST/$b/$h"
    done
done

# Fail loudly rather than leaving a stale tree: if a plant file includes a
# header that did not get copied, the QEMU build breaks in a way that looks
# like a QEMU problem instead of a missing sync rule.
missing=0
for f in "$DST"/*.c "$DST"/*.h; do
    while read -r h; do
        case "$h" in
            board_pins.h|pyro_sense.h) continue ;;   # per-board, handled above
        esac
        [ -f "$DST/$h" ] || { echo "sync-plant: $(basename "$f") includes $h, which is not synced" >&2; missing=1; }
    done < <(sed -nE 's/^#include "([^"]+)".*/\1/p' "$f")
done
[ "$missing" = 0 ] || exit 1
echo "synced plant + 3 board pin maps into $DST"
