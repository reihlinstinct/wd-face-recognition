#!/bin/sh
# shellcheck disable=SC2034
# Minimal device-side helpers for the wd-face-recognition OS 5 app.
# Sourced (no arguments) by the lifecycle scripts after they set:
#   APP_DIR          - directory the running script lives in (OS 5 passes it as $1)
#   APP_SCRIPT       - basename of the running script
#   APP_UPLOAD_PATH  - install.sh only: temporary upload directory ($1)
#   APP_TARGET_PATH  - install.sh only: final install path ($2)

LOG="/tmp/debug_apkg"

APP_PATH="$(cd "${APP_DIR}" && pwd)"

# Reads "Key: value" entries from apkg.rc (whitespace-tolerant, first match).
_apkg_var() {
	awk -v key="$1" '
		index($0, key ":") == 1 {
			sub(/^[^:]*:[ \t]*/, "")
			sub(/[ \t]*$/, "")
			print
			exit
		}
	' "${APP_PATH}/apkg.rc"
}

APP_NAME="$(_apkg_var Package)"
APP_VERSION="$(_apkg_var Version)"
APP_WEB_PATH="/var/www/${APP_NAME}"

if [ "${APP_SCRIPT}" != "install.sh" ]; then
	APPS_PATH="$(dirname "${APP_PATH}")"
	APP_UPLOAD_PATH="${APPS_PATH}/_install/${APP_NAME}"
fi

# Persistent data (face DB, settings, reports) survives uninstall by design.
APP_PERSISTENT_DATA_PATH="${APPS_PATH:-${APP_TARGET_PATH}}/${APP_NAME}_conf"

log() {
	printf '%s %s/%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" \
		"${APP_NAME}" "${APP_SCRIPT}" "$*" >> "${LOG}"
}
