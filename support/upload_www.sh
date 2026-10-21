#!/bin/bash
# Upload the web files to a Pyro board, and VERSION as /www/version.txt, which
# the page shows as the web files' version. Exits non-zero if any upload
# failed.
# Usage: ./upload_www.sh [host]
#
# The board takes a POST only with X-Pyro: 1 (src/http_conn.c).

HOST=${1:-pyro.local}
ROOT="$(dirname "$0")/.."
DIR="$ROOT/www"
failed=0

# [WEB-API-21] A write while the board's cleanup erases is refused with 503
# and sent again a second later, for up to two minutes.
upload() { # name file
    echo "  /www/$1"
    for _ in $(seq 120); do
        code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "http://$HOST/www/$1" \
            -H "X-Pyro: 1" -H "Content-Type: application/octet-stream" \
            --data-binary "@$2" --max-time 30) || code=000
        [ "$code" = 503 ] || break
        sleep 1
    done
    case "$code" in
        2??) ;;
        *) echo "    failed (HTTP $code)"; failed=1 ;;
    esac
}

echo "Uploading web files to $HOST..."
for f in "$DIR"/*; do
    [ -f "$f" ] && upload "$(basename "$f")" "$f"
done
[ -f "$ROOT/VERSION" ] && upload version.txt "$ROOT/VERSION"

if [ $failed -ne 0 ]; then
    echo "Error: some files did not upload."
    exit 1
fi
echo "Done. Open http://$HOST/"
