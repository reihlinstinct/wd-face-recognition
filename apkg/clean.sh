#!/bin/sh
# shellcheck disable=SC2034
# Called on removal (and reinstall) to revert init.sh.
APP_DIR="$1"
APP_SCRIPT="$(basename "$0")"
# shellcheck disable=SC1091
. "${APP_DIR}/helpers.sh"

log "removing web path: ${APP_WEB_PATH}"
rm -rf "${APP_WEB_PATH}"
