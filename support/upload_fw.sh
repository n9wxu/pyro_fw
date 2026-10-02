#!/bin/bash
# OTA firmware upload to a Pyro board. Exits non-zero when the board refuses
# the image or does not answer.
# Usage: ./upload_fw.sh [path_to_bin] [host]
#
# The board takes a POST only with X-Pyro: 1 (src/http_conn.c).

BIN=${1:-build/pyro_fw_c_fota_image.bin}
HOST=${2:-pyro.local}

if [ ! -f "$BIN" ]; then
    echo "Error: $BIN not found. Build first, then pass the .bin path."
    exit 1
fi

SIZE=$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN" 2>/dev/null)
echo "Uploading $BIN ($SIZE bytes) to $HOST..."

if ! curl --fail-with-body -sS -X POST "http://$HOST/api/ota" \
    --data-binary "@$BIN" \
    -H "Content-Type: application/octet-stream" \
    -H "X-Pyro: 1" \
    --max-time 120; then
    echo ""
    echo "Error: the board did not take the image."
    exit 1
fi

echo ""
echo "Device will reboot with new firmware."
