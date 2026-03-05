#!/bin/bash
# Upload web files to Pyro MK1B device
# Usage: ./upload_www.sh [host]

HOST=${1:-pyro.local}
DIR="$(dirname "$0")/www"

echo "Uploading web files to $HOST..."

for f in "$DIR"/*; do
    name=$(basename "$f")
    echo "  /www/$name"
    curl -s -X POST "http://$HOST/www/$name" --data-binary "@$f"
done

echo "Done. Open http://$HOST/"
