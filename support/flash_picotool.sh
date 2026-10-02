#!/bin/bash
# Flash a Pyro board via picotool (no debugger or BOOTSEL button needed)
# Usage: ./flash_picotool.sh [build_dir]
#
# The application is named for its board (pyro_fw_<board>.uf2).

BUILD=${1:-build}
# picotool on PATH, else the newest the VS Code Pico extension installed.
PICOTOOL=$(command -v picotool || ls -d ~/.pico-sdk/picotool/*/picotool/picotool 2>/dev/null | sort | tail -1)
if [ -z "$PICOTOOL" ]; then
    echo "Error: picotool not found on PATH or under ~/.pico-sdk"
    exit 1
fi
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

# The reply timeout is -W on Linux, where -t is the TTL, and -t on macOS.
if [ "$(uname)" = Linux ]; then WAIT=-W; else WAIT=-t; fi
echo "Done. Waiting for network..."
for i in $(seq 1 15); do
    sleep 1
    ping -c 1 $WAIT 2 192.168.7.1 >/dev/null 2>&1 && { echo "Device up after ${i}s"; exit 0; }
done
echo "Warning: device not responding on network"
