#!/bin/sh
# shellcheck disable=SC2034
# Called after install.sh (and on reinstall): prepare the web path.
APP_DIR="$1"
APP_SCRIPT="$(basename "$0")"
# shellcheck disable=SC1091
. "${APP_DIR}/helpers.sh"

log "creating web path: ${APP_WEB_PATH}"
mkdir -p "${APP_WEB_PATH}"

log "linking web pages from ${APP_PATH}/web to ${APP_WEB_PATH}"
ln -sf "${APP_PATH}/web"/* "${APP_WEB_PATH}/" >> "${LOG}" 2>&1

log "linking app icon into ${APP_WEB_PATH}"
ln -sf "${APP_PATH}/logo.svg" "${APP_WEB_PATH}/logo.svg" >> "${LOG}" 2>&1
