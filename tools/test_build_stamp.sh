#!/bin/bash
# cmake/BuildStamp.cmake: the stamp names the commit that is built, marks an
# uncommitted tree, carries the PREVIEW number, and rewrites its header only
# when the content changes (so a rebuild does not recompile everything).
#
#   tools/test_build_stamp.sh [cmake]
#
# Runs the script against a throwaway git repository; no game build needed.
set -eu
CMAKE=${1:-cmake}
SCRIPT=$(cd "$(dirname "$0")/.." && pwd)/cmake/BuildStamp.cmake
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
REPO=$T/repo; OUT=$T/mm_build_stamp.h
mkdir -p "$REPO"
git -C "$REPO" init -q
git -C "$REPO" -c user.name=t -c user.email=t@example.com commit -q --allow-empty -m one
stamp() { "$CMAKE" -DSOURCE_DIR="$REPO" -DOUTPUT="$OUT" -P "$SCRIPT"; }
fail() { echo "FAIL: $*"; exit 1; }

stamp
c1=$(git -C "$REPO" rev-parse --short=12 HEAD)
grep -q "MM_GIT_COMMIT \"$c1\"" "$OUT" || fail "commit: $(cat "$OUT")"
grep -q 'MM_PREVIEW ""' "$OUT" || fail "no PREVIEW file should give an empty preview"

# Unchanged content: the header is not rewritten.
touch -d '2000-01-01' "$OUT"
stamp
[ "$(stat -c %Y "$OUT")" = "$(date -d '2000-01-01' +%s)" ] || fail "header rewritten without a change"

# A new commit without reconfiguring: the stamp follows it.
git -C "$REPO" -c user.name=t -c user.email=t@example.com commit -q --allow-empty -m two
stamp
c2=$(git -C "$REPO" rev-parse --short=12 HEAD)
[ "$c1" != "$c2" ] && grep -q "MM_GIT_COMMIT \"$c2\"" "$OUT" || fail "stamp did not follow the commit"

# Uncommitted changes to a tracked file: marked with "+".
echo 27 > "$REPO/PREVIEW"
git -C "$REPO" add PREVIEW
git -C "$REPO" -c user.name=t -c user.email=t@example.com commit -q -m preview
stamp
grep -q 'MM_PREVIEW "27"' "$OUT" || fail "preview: $(cat "$OUT")"
echo 28 > "$REPO/PREVIEW"
stamp
grep -q "MM_GIT_COMMIT \"$(git -C "$REPO" rev-parse --short=12 HEAD)+\"" "$OUT" || fail "dirty tree not marked: $(cat "$OUT")"
grep -q 'MM_PREVIEW "28"' "$OUT" || fail "preview should be read from the working tree"
echo "build stamp ok"
