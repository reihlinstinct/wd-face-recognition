#!/bin/sh
# preinst.sh quiesces (then terminates) a running indexer before the
# package's binaries are replaced (review #101).
set -eu
cd "$(dirname "$0")/.."

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() {
    echo "preinst_test: $1" >&2
    exit 1
}

# A fake long-running indexer: any binary exec'd under that name is
# what pidof reports.
cp "$(command -v sleep)" "$WORK/wdf_indexer"
"$WORK/wdf_indexer" 500 &
FAKE_PID=$!

START=$(date +%s)
WDF_QUIESCE_MAX_SECS=2 sh preinst.sh "$PWD" || fail "preinst exits nonzero"
ELAPSED=$(( $(date +%s) - START ))
kill -0 "$FAKE_PID" 2>/dev/null && fail "runaway indexer survived preinst"
[ "$ELAPSED" -ge 2 ] || fail "preinst did not wait for the runaway indexer"

# Fast path: nothing running - no wait at all.
START=$(date +%s)
WDF_QUIESCE_MAX_SECS=5 sh preinst.sh "$PWD" || fail "preinst exits nonzero (idle)"
ELAPSED=$(( $(date +%s) - START ))
[ "$ELAPSED" -lt 2 ] || fail "preinst waits when nothing is running"

echo "preinst_test: all assertions passed"
