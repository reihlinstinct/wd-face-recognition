#!/bin/sh
# preinst.sh quiesces (then terminates) a running indexer before the
# package's binaries are replaced (review #101).
#
# Isolation (review #101): preinst.sh discovers processes through
# $WDF_PIDOF, and this test injects a fake that only ever reports the
# PIDs the test itself started - a real wdf_indexer/wdf_faces on a
# dev/CI machine is invisible to the script under test and can never
# be signaled by it.
set -eu
cd "$(dirname "$0")/.."

WORK=$(mktemp -d)
export WDF_FAKE_PIDS="$WORK/fake.pids"

cleanup() {
    # Reap every fake the test started, even on an early failure: the
    # SIGTERM-ignoring fake would otherwise outlive the test.
    if [ -f "$WDF_FAKE_PIDS" ]; then
        while read -r pid _name; do
            kill -9 "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        done < "$WDF_FAKE_PIDS"
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
    echo "preinst_test: $1" >&2
    exit 1
}

# The fake pidof: report only recorded PIDs that are still alive, and
# only when asked for their recorded name. Exit 1 when nothing
# matches, like real pidof.
cat > "$WORK/pidof" <<'EOS'
#!/bin/sh
found=1
[ -f "$WDF_FAKE_PIDS" ] || exit 1
for name in "$@"; do
    while read -r pid pname; do
        if [ "$pname" = "$name" ] && kill -0 "$pid" 2>/dev/null; then
            printf '%s ' "$pid"
            found=0
        fi
    done < "$WDF_FAKE_PIDS"
done
exit $found
EOS
chmod +x "$WORK/pidof"
export WDF_PIDOF="$WORK/pidof"

register_fake() {  # pid name
    echo "$1 $2" >> "$WDF_FAKE_PIDS"
}

# A fake long-running indexer: any binary exec'd under that name is
# what pidof reports.
cp "$(command -v sleep)" "$WORK/wdf_indexer"
"$WORK/wdf_indexer" 500 &
FAKE_PID=$!
register_fake "$FAKE_PID" wdf_indexer

START=$(date +%s)
WDF_QUIESCE_MAX_SECS=2 sh preinst.sh "$PWD" || fail "preinst exits nonzero"
ELAPSED=$(( $(date +%s) - START ))
kill -0 "$FAKE_PID" 2>/dev/null && fail "runaway indexer survived preinst"
[ "$ELAPSED" -ge 2 ] || fail "preinst did not wait for the runaway indexer"

# SIGTERM comes before SIGKILL (review #101 / issue #104): a fake that
# records the TERM and exits on it must see the marker written, and no
# KILL must be needed. A mutant that jumps straight to SIGKILL leaves
# the marker absent.
cp "$(command -v sh)" "$WORK/wdf_indexer.polite"
"$WORK/wdf_indexer.polite" -c 'trap "touch '"$WORK"'/termed; exit 0" TERM; while :; do sleep 1; done' &
POLITE_PID=$!
register_fake "$POLITE_PID" wdf_indexer
rm -f "$WORK/termed"
WDF_QUIESCE_MAX_SECS=1 sh preinst.sh "$PWD" || fail "preinst exits nonzero (TERM path)"
kill -0 "$POLITE_PID" 2>/dev/null && fail "polite fake survived its TERM trap"
[ -e "$WORK/termed" ] || fail "SIGKILL came before SIGTERM (no TERM marker)"

# Escalation path: a fake indexer that ignores SIGTERM must be taken
# down by SIGKILL; the install proceeds only once nothing survives.
cp "$(command -v sh)" "$WORK/wdf_faces"
"$WORK/wdf_faces" -c 'trap "" TERM; while :; do sleep 1; done' &
STUBBORN_PID=$!
register_fake "$STUBBORN_PID" wdf_faces
WDF_QUIESCE_MAX_SECS=1 sh preinst.sh "$PWD" || fail "preinst exits nonzero (escalation)"
kill -0 "$STUBBORN_PID" 2>/dev/null && fail "SIGTERM-ignoring process survived preinst"

# Grace period is real (reviewer): a fake that exits voluntarily after
# ~2s must never be signaled, and preinst must have waited for it.
cp "$(command -v sh)" "$WORK/wdf_indexer.grace"
"$WORK/wdf_indexer.grace" -c 'trap "touch '"$WORK"'/signaled" TERM; sleep 2' &
WILLING_PID=$!
register_fake "$WILLING_PID" wdf_indexer
rm -f "$WORK/signaled"
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
