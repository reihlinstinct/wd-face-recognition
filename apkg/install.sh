#!/bin/sh
# shellcheck disable=SC2034
# Called first on (re)installation: move the uploaded files to the install path.
APP_DIR="$1"
APP_SCRIPT="$(basename "$0")"
APP_UPLOAD_PATH="$1"
APP_TARGET_PATH="$2"
# shellcheck disable=SC1091
. "${APP_DIR}/helpers.sh"

log "moving files from ${APP_UPLOAD_PATH} to ${APP_TARGET_PATH}"
mv -f "${APP_UPLOAD_PATH}" "${APP_TARGET_PATH}"
