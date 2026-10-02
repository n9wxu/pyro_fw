#!/bin/bash
# Generate version.h from VERSION, which only a release changes
# (scripts/bump_version.sh, or CI's patch release).
# A build outside CI carries "+local" so it is never mistaken for the release
# of the same number.
set -eu
OUT="$1"
VERSION=$(cat "$2/VERSION" 2>/dev/null || echo "0.0.0")
[ -n "${CI_BUILD:-}" ] || VERSION="$VERSION+local"

BUILD_DATE=$(date +"%Y-%m-%d %H:%M:%S")
cat > "$OUT" << EOF
#ifndef VERSION_H
#define VERSION_H
#define FW_VERSION "$VERSION"
#define FW_BUILD_DATE "$BUILD_DATE"
#endif
EOF
