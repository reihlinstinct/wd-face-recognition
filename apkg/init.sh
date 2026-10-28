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

# Default indexer/faces config (issue #56): materialize once, never
# overwrite - a reinstall must keep the owner's edits. roots stays
# EMPTY on purpose (owner rule: family photos are private, nothing is
# scanned until roots are chosen in wfi.conf).
WFI_CONF="${APP_PATH}/wfi.conf"
if [ ! -f "${WFI_CONF}" ]; then
	log "writing default wfi.conf (roots empty until the owner sets them)"
	mkdir -p "${APP_PERSISTENT_DATA_PATH}/data"
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
