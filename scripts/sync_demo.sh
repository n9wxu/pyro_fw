#!/bin/bash
# Sync www/ files to docs/app/ for GitHub Pages demo.
# Run after modifying www/ files.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
mkdir -p "$ROOT/docs/app/www"
cp "$ROOT/www/"* "$ROOT/docs/app/www/"
# Make API paths relative for GitHub Pages
sed 's|/www/|www/|g' "$ROOT/www/index.html" > "$ROOT/docs/app/index.html"
sed -i.bak "s|'/api/|'api/|g; s|\"/api/|\"/api/|g" "$ROOT/docs/app/www/app.js"
sed -i.bak "s|'/api/|'api/|g" "$ROOT/docs/app/www/app.js"
rm -f "$ROOT/docs/app/www/app.js.bak"
echo "Synced www/ → docs/app/"
