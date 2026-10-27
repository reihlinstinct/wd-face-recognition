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
self_check "${APP_PATH}/bin/wdf_demo"

# Demo runner auth: a per-install PIN written to the reports share. Reading
# it needs the authenticated WD dashboard (or SMB) - the same independent
# channel as the TLS fingerprint - so a bare LAN browser cannot run the demo.
PIN_FILE="/shares/facerecog-reports/demo-pin.txt"
if [ ! -s "${PIN_FILE}" ]; then
	mkdir -p /shares/facerecog-reports
	dd if=/dev/urandom bs=6 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n' > "${PIN_FILE}"
	log "demo PIN written to ${PIN_FILE}"
fi

if [ "${rc}" -ne 0 ]; then
	log "one or more binary self-checks failed"
fi
exit "${rc}"
