#!/bin/sh
# Sandbox test for init.sh's wfi.conf materialization (review #87,
# hardened in #90). Runs init.sh against a fake app dir under a
# run-owned scratch dir (trap-cleaned) and asserts all four config
# states. The copied helpers.sh is redirected: APP_WEB_PATH and LOG
# point INSIDE the sandbox, so no run can touch /var/www or the real
# debug log even as root.
set -u
fails=0
bad() { fails=$((fails + 1)); echo "FAIL: $1" >&2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

REPO="$(cd "$(dirname "$0")/.." && pwd)"
CASE=0

setup() { # fresh fake app dir inside $WORK; sets APP and PERSIST
	CASE=$((CASE + 1))
	D="$WORK/case$CASE"
	mkdir -p "$D/apps/wd-face-recognition/web" "$D/www"
	cp "$REPO/apkg.rc" "$REPO/helpers.sh" "$REPO/init.sh" "$REPO/logo.svg" \
		"$D/apps/wd-face-recognition/"
	echo '<?php // stub' > "$D/apps/wd-face-recognition/web/index.php"
	# Redirect the web path and the log into the sandbox (issue #90):
	# an unmodified copy points at the host's /var/www and /tmp log.
	sed -i "s|^APP_WEB_PATH=.*|APP_WEB_PATH=\"$D/www\"|" \
		"$D/apps/wd-face-recognition/helpers.sh"
	sed -i "s|^LOG=.*|LOG=\"$D/debug_apkg\"|" \
		"$D/apps/wd-face-recognition/helpers.sh"
	APP="$D/apps/wd-face-recognition"
	PERSIST="$D/apps/wd-face-recognition_conf"
}

# linkok <app> <persist>: the app-dir wfi.conf is a symlink RESOLVING
# to the persistent config (review #90: -L alone is not enough).
linkok() {
	[ -L "$1/wfi.conf" ] && [ "$(readlink "$1/wfi.conf")" = "$2/wfi.conf" ]
}

# --- case 1: fresh install writes the default -----------------------------
setup
sh "$APP/init.sh" "$APP" >/dev/null 2>&1
[ -f "$PERSIST/wfi.conf" ] || bad 'default config written to the persistent path'
linkok "$APP" "$PERSIST" || bad 'case 1: symlink resolves to the persistent config'
grep -q '^roots=$' "$PERSIST/wfi.conf" || bad 'default roots empty'

# --- case 2: hand-made app-dir config is ADOPTED --------------------------
setup
printf 'roots=/shares/Mine\ndata_dir=/data\n' > "$APP/wfi.conf"
sh "$APP/init.sh" "$APP" >/dev/null 2>&1
linkok "$APP" "$PERSIST" || bad 'case 2: symlink resolves to the persistent config'
grep -q '^roots=/shares/Mine$' "$PERSIST/wfi.conf" || bad 'adopted config kept its roots'
sh "$APP/init.sh" "$APP" >/dev/null 2>&1
grep -q '^roots=/shares/Mine$' "$PERSIST/wfi.conf" || bad 'second run rewrote the adopted config'

# --- case 3: both exist - persistent wins, real file backed up ------------
setup
mkdir -p "$PERSIST"
printf 'roots=/shares/Persistent\n' > "$PERSIST/wfi.conf"
printf 'roots=/shares/AppDir\n' > "$APP/wfi.conf"
sh "$APP/init.sh" "$APP" >/dev/null 2>&1
linkok "$APP" "$PERSIST" || bad 'case 3: symlink resolves to the persistent config'
grep -q '^roots=/shares/Persistent$' "$PERSIST/wfi.conf" || bad 'persistent config lost'
ls "$PERSIST"/wfi.conf.bak-* >/dev/null 2>&1 || bad 'no .bak next to the persistent config'
grep -q '^roots=/shares/AppDir$' "$PERSIST"/wfi.conf.bak-* 2>/dev/null || bad '.bak does not hold the app-dir config'

# --- case 4: persistent only - untouched, link created --------------------
setup
mkdir -p "$PERSIST"
printf 'roots=/shares/Keep\n' > "$PERSIST/wfi.conf"
sh "$APP/init.sh" "$APP" >/dev/null 2>&1
linkok "$APP" "$PERSIST" || bad 'case 4: symlink resolves to the persistent config'
grep -q '^roots=/shares/Keep$' "$PERSIST/wfi.conf" || bad 'persistent config modified'

# --- case 5: fault injection - a failing mv leaves no partial state ------
# (review #91: without this the old non-atomic direct write would pass
# the whole suite). A PATH-stubbed mv fails; init.sh must exit 1 with
# no wfi.conf and no leftover temp file, and the next normal run must
# recover.
setup
STUB="$WORK/stub"
mkdir -p "$STUB"
printf '#!/bin/sh\nexit 1\n' > "$STUB/mv"
chmod +x "$STUB/mv"
PATH="$STUB:$PATH" sh "$APP/init.sh" "$APP" >/dev/null 2>&1
rc=$?
[ "$rc" -eq 1 ] || bad "failing mv: init.sh exits 1 (got $rc)"
[ ! -f "$PERSIST/wfi.conf" ] || bad 'failing mv: no wfi.conf installed'
ls "$PERSIST"/wfi.conf.tmp.* >/dev/null 2>&1 && bad 'failing mv: temp file left behind'
sh "$APP/init.sh" "$APP" >/dev/null 2>&1
rc=$?
[ "$rc" -eq 0 ] || bad 'next run recovers'
grep -q '^roots=$' "$PERSIST/wfi.conf" || bad 'recovery wrote the default config'

# --- no stray temp dirs: the trap removes $WORK, checked by the caller ----
if [ "$fails" -gt 0 ]; then
	echo "$fails check(s) failed" >&2
	exit 1
fi
echo "init_conf_test: all checks passed"
