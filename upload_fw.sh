#!/bin/bash
# OTA firmware upload to Pyro MK1B device (A/B bootloader)
# Usage: ./upload_fw.sh [path_to_bin] [host]

BIN=${1:-build/pyro_fw_c_fota_image.bin}
HOST=${2:-192.168.7.1}

if [ ! -f "$BIN" ]; then
    echo "Error: $BIN not found. Build first, then pass the .bin path."
    exit 1
fi

SIZE=$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN" 2>/dev/null)
echo "Uploading $BIN ($SIZE bytes) to $HOST..."

curl -X POST "http://$HOST/api/ota" \
    --data-binary "@$BIN" \
    -H "Content-Type: application/octet-stream" \
    --max-time 120

echo ""
echo "Device will reboot with new firmware."
