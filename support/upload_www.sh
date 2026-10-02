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

upload() { # name file
    echo "  /www/$1"
    curl --fail-with-body -sS -o /dev/null -X POST "http://$HOST/www/$1" \
        -H "X-Pyro: 1" -H "Content-Type: application/octet-stream" \
        --data-binary "@$2" --max-time 30 || { echo "    failed"; failed=1; }
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
