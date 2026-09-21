#!/bin/bash
# Flash Pyro MK1B/MK1C via picotool (no debugger or BOOTSEL button needed)
# Usage: ./flash_picotool.sh [build_dir]
#
# The application artifact is named per board (pyro_fw_mk1b.uf2 /
# pyro_fw_mk1c.uf2) so an image cannot be flashed to the wrong hardware.

BUILD=${1:-build}
PICOTOOL=~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool
BL="$BUILD/_deps/pico_fota_bootloader-build/pico_fota_bootloader.uf2"
APP=$(ls "$BUILD"/pyro_fw_mk1*.uf2 2>/dev/null | head -1)
if [ -z "$APP" ]; then
    echo "Error: no pyro_fw_mk1*.uf2 found in $BUILD"
    echo "Build first:  cmake -B $BUILD -DPYRO_BOARD=mk1b|mk1c && cmake --build $BUILD"
    exit 1
fi
echo "Application: $(basename "$APP")"

for f in "$BL" "$APP"; do
    [ -f "$f" ] || { echo "Error: $f not found"; exit 1; }
done

# Already in BOOTSEL (RPI-RP2 mounted)? Then skip the forced reboot, which
# only applies to a device currently running the application.
if $PICOTOOL info >/dev/null 2>&1; then
    echo "Device already in BOOTSEL."
else
    echo "Forcing BOOTSEL..."
    $PICOTOOL reboot -u -f --vid 0x2E8A --pid 0x4002 || { echo "Error: device not found"; exit 1; }
    sleep 2
fi

echo "Loading bootloader..."
$PICOTOOL load "$BL" || exit 1

echo "Loading application..."
$PICOTOOL load "$APP" || exit 1

echo "Rebooting to application..."
$PICOTOOL reboot || exit 1

echo "Done. Waiting for network..."
for i in $(seq 1 15); do
    sleep 1
    ping -c 1 -t 2 192.168.7.1 >/dev/null 2>&1 && { echo "Device up after ${i}s"; exit 0; }
done
echo "Warning: device not responding on network"
