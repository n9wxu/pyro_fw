#!/bin/bash
# Bump the version and release it.
#
#   ./scripts/bump_version.sh minor      2.2.7 -> 2.3.0
#   ./scripts/bump_version.sh major      2.2.7 -> 3.0.0
#   ./scripts/bump_version.sh patch      2.2.7 -> 2.2.8  (CI does this itself)
#
# A patch bump is automatic: CI cuts one from every green push to main, so
# this script is really for the deliberate ones. It is here for patch too
# so there is one way to do it when CI cannot.
#
# What it does NOT do is decide anything. support/version.py owns the rules
# -- a bump zeroes everything below it -- and refuses a step that breaks
# them, so this cannot produce a 2.3.7.
set -e

LEVEL="${1:-}"
case "$LEVEL" in
    major|minor|patch) ;;
    *) echo "usage: $0 major|minor|patch" >&2; exit 1 ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# A bump is a release, and a release of a dirty tree is a release of
# something that is not in the history.
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "working tree has uncommitted changes; commit or stash them first" >&2
    exit 1
fi

BRANCH=$(git rev-parse --abbrev-ref HEAD)
if [ "$BRANCH" != "main" ]; then
    echo "on '$BRANCH', not main: a release comes from main" >&2
    exit 1
fi

git fetch -q origin
if [ "$(git rev-parse HEAD)" != "$(git rev-parse origin/main)" ]; then
    echo "HEAD is not origin/main; pull or push first" >&2
    exit 1
fi

OLD=$(python3 support/version.py --current)
NEW=$(python3 support/version.py --next "$LEVEL")

if git rev-parse -q --verify "refs/tags/v$NEW" >/dev/null; then
    echo "tag v$NEW already exists" >&2
    exit 1
fi

echo "$OLD -> $NEW ($LEVEL)"
python3 support/version.py --set "$NEW" >/dev/null

git add VERSION
# [skip ci] so the push does not start a build that would then bump again.
# The tag is what runs the release.
git commit -q -m "chore(release): v$NEW [skip ci]"
git tag -a "v$NEW" -m "v$NEW"

echo "pushing main and v$NEW"
git push -q origin main
git push -q origin "v$NEW"
echo "done -- the Release workflow runs from the tag"
