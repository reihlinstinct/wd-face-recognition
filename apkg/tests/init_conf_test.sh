#!/bin/sh
# Sandbox test for init.sh's wfi.conf materialization (review #87).
# Runs init.sh against a fake app dir under /tmp and asserts all four
# config states: fresh default, adoption of a hand-made config, the
# both-exist backup, and persistent-only idempotency. /var/www writes
# fail harmlessly in the sandbox (init.sh only logs them).
set -u
fails=0
bad() { fails=$((fails + 1)); echo "FAIL: $1" >&2; }

REPO="$(cd "$(dirname "$0")/.." && pwd)"

setup() { # fresh fake app dir; echoes its path
	D="$(mktemp -d)"
	mkdir -p "$D/apps/wd-face-recognition/web"
	cp "$REPO/apkg.rc" "$REPO/helpers.sh" "$REPO/init.sh" "$REPO/logo.svg" \
		"$D/apps/wd-face-recognition/"
	echo '<?php // stub' > "$D/apps/wd-face-recognition/web/index.php"
	echo "$D/apps/wd-face-recognition"
}

# --- case 1: fresh install writes the default -----------------------------
APP="$(setup)"
PERSIST="$(dirname "$APP")/wd-face-recognition_conf"
sh "$APP/init.sh" "$APP" >/dev/null 2>&1
[ -f "$PERSIST/wfi.conf" ] || bad 'default config written to the persistent path'
[ -L "$APP/wfi.conf" ] || bad 'app-dir wfi.conf is a symlink'
grep -q '^roots=$' "$PERSIST/wfi.conf" || bad 'default roots empty'

# --- case 2: hand-made app-dir config is ADOPTED --------------------------
APP="$(setup)"
PERSIST="$(dirname "$APP")/wd-face-recognition_conf"
printf 'roots=/shares/Mine\ndata_dir=/data\n' > "$APP/wfi.conf"
sh "$APP/init.sh" "$APP" >/dev/null 2>&1
[ -L "$APP/wfi.conf" ] || bad 'adopted: app-dir wfi.conf is a symlink'
grep -q '^roots=/shares/Mine$' "$PERSIST/wfi.conf" || bad 'adopted config kept its roots'
sh "$APP/init.sh" "$APP" >/dev/null 2>&1
grep -q '^roots=/shares/Mine$' "$PERSIST/wfi.conf" || bad 'second run rewrote the adopted config'

# --- case 3: both exist - persistent wins, real file backed up ------------
APP="$(setup)"
PERSIST="$(dirname "$APP")/wd-face-recognition_conf"
mkdir -p "$PERSIST"
printf 'roots=/shares/Persistent\n' > "$PERSIST/wfi.conf"
printf 'roots=/shares/AppDir\n' > "$APP/wfi.conf"
sh "$APP/init.sh" "$APP" >/dev/null 2>&1
[ -L "$APP/wfi.conf" ] || bad 'both-exist: app-dir wfi.conf is a symlink'
grep -q '^roots=/shares/Persistent$' "$PERSIST/wfi.conf" || bad 'persistent config lost'
ls "$PERSIST"/wfi.conf.bak-* >/dev/null 2>&1 || bad 'no .bak next to the persistent config'
grep -q '^roots=/shares/AppDir$' "$PERSIST"/wfi.conf.bak-* 2>/dev/null || bad '.bak does not hold the app-dir config'

# --- case 4: persistent only - untouched, link created --------------------
APP="$(setup)"
PERSIST="$(dirname "$APP")/wd-face-recognition_conf"
mkdir -p "$PERSIST"
printf 'roots=/shares/Keep\n' > "$PERSIST/wfi.conf"
sh "$APP/init.sh" "$APP" >/dev/null 2>&1
[ -L "$APP/wfi.conf" ] || bad 'persistent-only: app-dir wfi.conf is a symlink'
grep -q '^roots=/shares/Keep$' "$PERSIST/wfi.conf" || bad 'persistent config modified'

if [ "$fails" -gt 0 ]; then
	echo "$fails check(s) failed" >&2
	exit 1
fi
echo "init_conf_test: all checks passed"
