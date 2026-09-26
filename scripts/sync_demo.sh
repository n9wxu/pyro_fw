#!/bin/bash
# Sync www/ files to docs/app/ for the GitHub Pages demo. Run after modifying
# www/ files.
#
#   scripts/sync_demo.sh           write docs/app/
#   scripts/sync_demo.sh --check   exit 1 if docs/app/ is not what this would write
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."

sync_into() {
  local dest=$1
  mkdir -p "$dest/www"
  cp "$ROOT/www/"* "$dest/www/"
  # Make API paths relative for GitHub Pages
  sed 's|/www/|www/|g' "$ROOT/www/index.html" > "$dest/index.html"
  sed -i.bak "s|'/api/|'api/|g" "$dest/www/app.js"
  rm -f "$dest/www/app.js.bak"
}

if [ "$1" = "--check" ]; then
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  cp -R "$ROOT/docs/app/." "$tmp/"
  sync_into "$tmp"
  if ! diff -r "$ROOT/docs/app" "$tmp" > /dev/null; then
    diff -r "$ROOT/docs/app" "$tmp" | head -20
    echo "docs/app/ has drifted from www/: run scripts/sync_demo.sh" >&2
    exit 1
  fi
  echo "docs/app/ matches www/"
  exit 0
fi

sync_into "$ROOT/docs/app"
echo "Synced www/ → docs/app/"
