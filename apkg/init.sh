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

# Default indexer/faces config (issue #56). The file lives on the
# PERSISTENT _conf path - under ${APP_PATH} an uninstall would delete
# the owner's chosen roots (review #87). Never overwrite: a reinstall
# keeps owner edits. roots stays EMPTY on purpose (owner rule: family
# photos are private, nothing is scanned until roots are chosen).
# Abort if the data dir cannot be created: writing a config that
# points at an unwritable data_dir would fail every run later.
if ! mkdir -p "${APP_PERSISTENT_DATA_PATH}/data" >> "${LOG}" 2>&1; then
	log "ERROR: cannot create ${APP_PERSISTENT_DATA_PATH}/data - wfi.conf not written"
	exit 1
fi
WFI_CONF="${APP_PERSISTENT_DATA_PATH}/wfi.conf"
if [ ! -f "${WFI_CONF}" ]; then
	log "writing default wfi.conf (roots empty until the owner sets them)"
	{
		echo "# wd-face-recognition indexer/faces config."
		echo "# roots is EMPTY on purpose (privacy): nothing is scanned until"
		echo "# you list share paths, comma-separated. Example:"
		echo "#   roots=/shares/Photos,/shares/Family"
		echo "roots="
		echo "data_dir=${APP_PERSISTENT_DATA_PATH}/data"
		echo "models_dir=${APP_PATH}/models"
		echo "threads=2"
	} > "${WFI_CONF}"
fi
# The pages and CLI look for wfi.conf next to the app; link to the
# persistent copy so an uninstall only ever removes the link.
ln -sf "${WFI_CONF}" "${APP_PATH}/wfi.conf" >> "${LOG}" 2>&1
