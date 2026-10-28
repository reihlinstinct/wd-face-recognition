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
APP_CONF_LINK="${APP_PATH}/wfi.conf"
# Adopt a hand-made config that predates the persistent layout (review
# #87): NEVER replace a real file with the empty default.
if [ -f "${APP_CONF_LINK}" ] && [ ! -L "${APP_CONF_LINK}" ]; then
	if [ ! -f "${WFI_CONF}" ]; then
		log "adopting existing ${APP_CONF_LINK} as the persistent wfi.conf"
		if ! mv "${APP_CONF_LINK}" "${WFI_CONF}" >> "${LOG}" 2>&1; then
			log "ERROR: cannot move ${APP_CONF_LINK} to the persistent path"
			exit 1
		fi
	else
		BAK="${WFI_CONF}.bak-$(date '+%Y%m%d%H%M%S')"
		log "WARNING: both ${APP_CONF_LINK} and ${WFI_CONF} exist - keeping the persistent one, the app-dir file moves to ${BAK}"
		if ! mv "${APP_CONF_LINK}" "${BAK}" >> "${LOG}" 2>&1; then
			log "ERROR: cannot move ${APP_CONF_LINK} aside to ${BAK}"
			exit 1
		fi
	fi
fi
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
# persistent copy so an uninstall only ever removes the link. Any real
# file was adopted above, so only a stale symlink can be in the way.
[ -L "${APP_CONF_LINK}" ] && rm -f "${APP_CONF_LINK}"
if ! ln -s "${WFI_CONF}" "${APP_CONF_LINK}" >> "${LOG}" 2>&1; then
	log "ERROR: cannot link ${APP_CONF_LINK} to ${WFI_CONF}"
	exit 1
fi
