#!/bin/sh
# shellcheck disable=SC2034
# Called before reinstallation. Nothing to back up in the skeleton milestone.
APP_DIR="$1"
APP_SCRIPT="$(basename "$0")"
# shellcheck disable=SC1091
. "${APP_DIR}/helpers.sh"

log "pre-reinstall: nothing to do for the M1 skeleton"
