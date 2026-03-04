#!/bin/bash
# Generate version.h, auto-increment patch version on each build
OUT="$1"
VER_FILE="$2/VERSION"
VERSION=$(cat "$VER_FILE" 2>/dev/null || echo "1.0.0")
IFS='.' read -r MAJOR MINOR PATCH <<< "$VERSION"
PATCH=$((PATCH + 1))
VERSION="$MAJOR.$MINOR.$PATCH"
echo "$VERSION" > "$VER_FILE"
BUILD_DATE=$(date +"%Y-%m-%d %H:%M:%S")
cat > "$OUT" << EOF
#ifndef VERSION_H
#define VERSION_H
#define FW_VERSION "$VERSION"
#define FW_BUILD_DATE "$BUILD_DATE"
#endif
EOF
