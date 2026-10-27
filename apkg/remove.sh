#!/bin/sh
# shellcheck disable=SC2034
# Called on removal. Binaries and scripts are deleted; the persistent data
# directory (face DB, settings, reports) is kept by design (docs/DESIGN.md,
# Packaging section). A "remove data too" option arrives with the settings
# share in a later milestone.
APP_DIR="$1"
APP_SCRIPT="$(basename "$0")"
# shellcheck disable=SC1091
. "${APP_DIR}/helpers.sh"

log "removing app files from ${APP_PATH}; keeping ${APP_PERSISTENT_DATA_PATH}"
rm -rf "${APP_PATH}"
