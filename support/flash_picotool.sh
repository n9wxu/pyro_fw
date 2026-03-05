#!/bin/bash
# Flash Pyro MK1B via picotool (no debugger or BOOTSEL button needed)
# Usage: ./flash_picotool.sh [build_dir]

BUILD=${1:-build}
PICOTOOL=~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool
BL="$BUILD/_deps/pico_fota_bootloader-build/pico_fota_bootloader.uf2"
APP="$BUILD/pyro_fw_c.uf2"

for f in "$BL" "$APP"; do
    [ -f "$f" ] || { echo "Error: $f not found"; exit 1; }
done

echo "Forcing BOOTSEL..."
$PICOTOOL reboot -u -f --vid 0x2E8A --pid 0x4002 || { echo "Error: device not found"; exit 1; }
sleep 2

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
