#!/bin/sh
# shellcheck disable=SC2034
# Called before reinstallation. The new package's binaries are about to
# replace the old ones, possibly while an OLD wdf_indexer/wdf_faces run
# is still active (review #101): a pre-flock build takes no flock, and
# the pid-file-era build even UNLINKS wdf.lock on exit - either way the
# new process's lock cannot exclude the old run, so the old mutator is
# waited out (then terminated) before the swap happens. The very first
# upgrade from a lock-less build cannot be guarded by flock at all;
# this wait is the only guard there.
APP_DIR="$1"
APP_SCRIPT="$(basename "$0")"
# shellcheck disable=SC1091
. "${APP_DIR}/helpers.sh"

QUIESCE_MAX="${WDF_QUIESCE_MAX_SECS:-60}"
n=0
while pidof wdf_indexer wdf_faces >/dev/null 2>&1; do
    n=$((n + 1))
    if [ "$n" -gt "$QUIESCE_MAX" ]; then
        log "wdf_indexer/wdf_faces still running after ${QUIESCE_MAX}s - terminating"
        # shellcheck disable=SC2046
        kill $(pidof wdf_indexer wdf_faces) 2>/dev/null
        sleep 2
        break
    fi
    sleep 1
done
log "pre-reinstall: no wdf_indexer/wdf_faces left running"
