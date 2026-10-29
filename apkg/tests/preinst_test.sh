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

# Escalation path (review #101): a fake indexer that ignores SIGTERM
# must be taken down by SIGKILL; the install proceeds only once nothing
# survives.
cp "$(command -v sh)" "$WORK/wdf_faces"
"$WORK/wdf_faces" -c 'trap "" TERM; while :; do sleep 1; done' &
STUBBORN_PID=$!
WDF_QUIESCE_MAX_SECS=1 sh preinst.sh "$PWD" || fail "preinst exits nonzero (escalation)"
kill -0 "$STUBBORN_PID" 2>/dev/null && fail "SIGTERM-ignoring process survived preinst"

# Grace period is real (reviewer): a fake that exits voluntarily after
# ~2s must never be signaled, and preinst must have waited for it.
cp "$(command -v sh)" "$WORK/wdf_indexer"
"$WORK/wdf_indexer" -c 'trap "touch '"$WORK"'/signaled" TERM; sleep 2' &
WILLING_PID=$!
START=$(date +%s)
WDF_QUIESCE_MAX_SECS=10 sh preinst.sh "$PWD" || fail "preinst exits nonzero (grace)"
ELAPSED=$(( $(date +%s) - START ))
[ -e "$WORK/signaled" ] && fail "voluntary exit was signaled anyway"
[ "$ELAPSED" -ge 1 ] || fail "preinst did not wait out the grace period"
kill -0 "$WILLING_PID" 2>/dev/null && fail "voluntary fake still running"

# Fast path: nothing running - no wait at all.
START=$(date +%s)
WDF_QUIESCE_MAX_SECS=5 sh preinst.sh "$PWD" || fail "preinst exits nonzero (idle)"
ELAPSED=$(( $(date +%s) - START ))
[ "$ELAPSED" -lt 2 ] || fail "preinst waits when nothing is running"

echo "preinst_test: all assertions passed"
