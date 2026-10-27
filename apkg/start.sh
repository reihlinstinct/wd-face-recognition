#!/bin/sh
# shellcheck disable=SC2034
# Called when the app is enabled. The M1 skeleton runs no daemon; its start
# action proves the statically linked binaries execute on the device, and
# fails when any self-check fails.
APP_DIR="$1"
APP_SCRIPT="$(basename "$0")"
# shellcheck disable=SC1091
. "${APP_DIR}/helpers.sh"

log "enabling ${APP_NAME} ${APP_VERSION} (M1 skeleton: no daemon yet)"

rc=0
self_check() {
	if [ -x "$1" ]; then
		out="$("$1" --version 2>&1)" || rc=1
		log "self-check ${1##*/}: ${out}"
	else
		log "ERROR: ${1##*/} missing or not executable"
		rc=1
	fi
}
self_check "${APP_PATH}/bin/wdf-worker"
self_check "${APP_PATH}/bin/wdf-service"

if [ "${rc}" -ne 0 ]; then
	log "one or more binary self-checks failed"
fi
exit "${rc}"
