#!/bin/sh
# shellcheck disable=SC2034
# Called when the app is disabled. The M1 skeleton runs no daemon.
APP_DIR="$1"
APP_SCRIPT="$(basename "$0")"
# shellcheck disable=SC1091
. "${APP_DIR}/helpers.sh"

log "disabling ${APP_NAME} (M1 skeleton: no daemon to stop)"
