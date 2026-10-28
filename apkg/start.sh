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
# Validate shape, not just non-emptiness: a partial write (1-11 chars) or
# non-hex content must trigger regeneration, and the generated PIN must land
# as exactly 12 lowercase hex chars. busybox grep supports -q -x -E.
pin_valid() { grep -qxE '[0-9a-f]{12}' "${PIN_FILE}" 2>/dev/null; }
if ! pin_valid; then
	mkdir -p /shares/facerecog-reports
	# busybox-safe generator (tr/head only): the previous dd|od -An -tx1
	# pipeline left a 0-byte PIN on the PR4100 - the redirect truncates the
	# file before the pipeline runs, and busybox od rejected the flags.
	tr -dc '0-9a-f' < /dev/urandom | head -c 12 > "${PIN_FILE}"
	if pin_valid; then
		log "demo PIN written to ${PIN_FILE}"
	else
		log "ERROR: failed to generate demo PIN at ${PIN_FILE}"
		rc=1
	fi
fi

if [ "${rc}" -ne 0 ]; then
	log "one or more binary self-checks failed"
fi
exit "${rc}"
