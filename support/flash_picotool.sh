#!/bin/bash
# Flash a Pyro board via picotool (no debugger or BOOTSEL button needed)
# Usage: ./flash_picotool.sh [build_dir]
# Runs on macOS, Linux, and Windows under Git Bash.
#
# The application is named for its board (pyro_fw_<board>.uf2).

BUILD=${1:-build}
# picotool on PATH, else the newest the VS Code Pico extension installed.
PICOTOOL=$(command -v picotool ||
    ls -d ~/.pico-sdk/picotool/*/picotool/picotool ~/.pico-sdk/picotool/*/picotool/picotool.exe 2>/dev/null |
    sort | tail -1)
if [ -z "$PICOTOOL" ]; then
    echo "Error: picotool not found on PATH or under ~/.pico-sdk"
    exit 1
fi
BL="$BUILD/_deps/pico_fota_bootloader-build/pico_fota_bootloader.uf2"
APP=$(ls "$BUILD"/pyro_fw_mk1*.uf2 2>/dev/null | head -1)
if [ -z "$APP" ]; then
    echo "Error: no pyro_fw_mk1*.uf2 found in $BUILD"
    echo "Build first:  cmake -B $BUILD -DPYRO_BOARD=<board> && cmake --build $BUILD"
    echo "              (mk1a, mk1b, mk1b_8mb, mk1c or mk1c_sd)"
    exit 1
fi
echo "Application: $(basename "$APP")"

for f in "$BL" "$APP"; do
    [ -f "$f" ] || { echo "Error: $f not found"; exit 1; }
done

# Already in BOOTSEL (RPI-RP2 mounted)? Then skip the forced reboot, which
# only applies to a device currently running the application.
if "$PICOTOOL" info >/dev/null 2>&1; then
    echo "Device already in BOOTSEL."
else
    echo "Forcing BOOTSEL..."
    "$PICOTOOL" reboot -u -f --vid 0x2E8A --pid 0x4002 || { echo "Error: device not found"; exit 1; }
    sleep 2
fi

echo "Loading bootloader..."
"$PICOTOOL" load "$BL" || exit 1

echo "Loading application..."
"$PICOTOOL" load "$APP" || exit 1

# A board's address follows its flash chip (DD-099), so the one to wait for
# is whichever link was not up while this board sat in BOOTSEL.
# ip and ifconfig print "inet 192.168.N.2"; Windows' ipconfig prints
# "IPv4 Address. . . : 192.168.N.2" with CRLF line ends.
boards() {
    { ip -4 -o addr 2>/dev/null || ifconfig 2>/dev/null || ipconfig 2>/dev/null; } | tr -d '\r' |
        sed -n -E 's/.*(inet |IPv4[^:]*: *)192\.168\.([0-9]+)\.2([^0-9].*)?$/192.168.\2.1/p' | sort -u
}
BEFORE=$(boards)

echo "Rebooting to application..."
"$PICOTOOL" reboot || exit 1

# One echo request with a 2 s reply timeout: -W on Linux, where -t is the TTL;
# -t on macOS; -n and -w in milliseconds on Windows.
case "$(uname)" in
    Linux) PING="ping -c 1 -W 2" ;;
    MINGW* | MSYS* | CYGWIN*) PING="ping -n 1 -w 2000" ;;
    *) PING="ping -c 1 -t 2" ;;
esac
echo "Done. Waiting for network..."
for i in $(seq 1 15); do
    sleep 1
    for ip in $(boards); do
        echo "$BEFORE" | grep -qxF "$ip" && continue
        $PING "$ip" >/dev/null 2>&1 && { echo "Device up at $ip after ${i}s"; exit 0; }
    done
done
echo "Warning: device not responding on network"
