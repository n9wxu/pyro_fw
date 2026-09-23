#!/bin/bash
# Run a Pyro firmware image on the emulated RP2040 with the board plant
# attached.
#
#   ./sim/qemu/run-qemu.sh build-mk1a
#   ./sim/qemu/run-qemu.sh build-mk1a match1=absent,match2=present
#   ./sim/qemu/run-qemu.sh build-mk1b match1=present,match-mohm=10000
#
# Needs a QEMU built from the RP2040 fork with the pyro devices; point
# PYRO_QEMU at its build directory (default ~/src/qemu-rp2040-pico/build).
set -e

BUILD="${1:?usage: run-qemu.sh <firmware-build-dir> [plant-options]}"
PLANT_OPTS="${2:-match1=present,match2=absent}"
QEMU="${PYRO_QEMU:-$HOME/src/qemu-rp2040-pico/build}/qemu-system-arm"
# The board name comes from the artifact, not from the directory name: a
# build tree can be called anything, and CMake names the image after the
# board it was configured for.
APP=$(ls "$BUILD"/pyro_fw_*.bin 2>/dev/null | grep -v fota_image | head -1)
[ -n "$APP" ] || { echo "no pyro_fw_*.bin in $BUILD -- build the firmware first" >&2; exit 1; }
BOARD=$(basename "$APP" .bin | sed 's/^pyro_fw_//')

BOOT="$BUILD/_deps/pico_fota_bootloader-build/pico_fota_bootloader.bin"
[ -f "$BOOT" ] || { echo "no bootloader at $BOOT" >&2; exit 1; }
echo "board: $BOARD"

# The flash image is assembled rather than passed with -kernel.
#
# -kernel wants an image with boot2 at 0x10000000, and the application is
# linked above that because pico_fota_bootloader owns everything below it.
# So the two pieces are laid into one raw array at their own offsets, which
# is what the real part holds.
#
# The array only has to cover what is written here. The machine's own flash
# is 16 MiB -- the full width of the XIP window -- and anything this image
# does not cover stays at the erased 0xff, so a board linked for a 2 MiB
# part and one linked for 16 MiB both work with no size argument.
APP_OFFSET=0xa000
IMG="${TMPDIR:-/tmp}/pyro_${BOARD}_flash.bin"

python3 - "$BOOT" "$APP" "$IMG" "$APP_OFFSET" <<'PY'
import sys
boot, app, out, off = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 0)
b = open(boot, 'rb').read()
a = open(app, 'rb').read()
img = bytearray(b'\xff' * (off + len(a)))
img[0:len(b)] = b
img[off:off + len(a)] = a           # the app slot the bootloader jumps to
open(out, 'wb').write(img)
print(f"flash image: bootloader {len(b)} B at 0x0, app {len(a)} B at {off:#x}")
PY

exec "$QEMU" \
  -machine "raspi-pico,flash-file=$IMG" \
  -device "pyro-plant,board=$BOARD,$PLANT_OPTS" \
  -serial stdio -display none -no-reboot
