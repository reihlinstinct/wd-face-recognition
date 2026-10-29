#!/bin/sh
# shellcheck disable=SC2034
# Called before reinstallation. The new package's binaries are about to
# replace the old ones, possibly while an OLD wdf_indexer/wdf_faces run
# is still active (review #101): a pre-flock build takes no flock, and
# the pid-file-era build even UNLINKS wdf.lock on exit - either way the
# new process's lock cannot exclude the old run, so the old mutator is
# waited out, then terminated with bounded SIGTERM/SIGKILL escalation,
# and a survivor aborts the upgrade instead of overlapping it. The very
# first upgrade from a lock-less build cannot be guarded by flock at
# all; this quiesce is the only guard there.
APP_DIR="$1"
APP_SCRIPT="$(basename "$0")"
# shellcheck disable=SC1091
. "${APP_DIR}/helpers.sh"

QUIESCE_MAX="${WDF_QUIESCE_MAX_SECS:-60}"
n=0
while pidof wdf_indexer wdf_faces >/dev/null 2>&1; do
    n=$((n + 1))
    if [ "$n" -le "$QUIESCE_MAX" ]; then
        sleep 1
        continue
    fi
    log "wdf_indexer/wdf_faces still running after ${QUIESCE_MAX}s - terminating"
    # shellcheck disable=SC2046
    kill $(pidof wdf_indexer wdf_faces) 2>/dev/null
    t=0
    while pidof wdf_indexer wdf_faces >/dev/null 2>&1; do
        t=$((t + 1))
        if [ "$t" -gt 5 ]; then
            log "still running after SIGTERM - escalating to SIGKILL"
            # shellcheck disable=SC2046
            kill -9 $(pidof wdf_indexer wdf_faces) 2>/dev/null
            sleep 1
            break
        fi
        sleep 1
    done
    if pidof wdf_indexer wdf_faces >/dev/null 2>&1; then
        log "ERROR: wdf_indexer/wdf_faces survived SIGKILL - refusing to upgrade over a live run"
        exit 1
    fi
    break
done
log "pre-reinstall: no wdf_indexer/wdf_faces left running"
