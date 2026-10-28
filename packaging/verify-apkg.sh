#!/usr/bin/env bash
# Verifies a built apkg .bin for My Cloud OS 5 (regression gate, issue #22).
#
# Catches the failures we hit during on-device install debugging:
#   - empty apkg.sign from mksapkg on an OpenSSL without Blowfish (the NAS
#     rejects it with "signature failed")
#   - payload tarball root dir != module name (the device probes
#     _install/<module>/apkg.sign, so a wrong dir means the signature file is
#     never found)
#   - header/apkg.rc inconsistencies, missing required fields, non-PNG icon
#
# Usage: packaging/verify-apkg.sh [path-to-.bin]
# Defaults to the single packages/MyCloudPR4100_*.bin produced by build-apkg.sh.
set -euo pipefail

MODULE="wd-face-recognition"
# Fixed platform signing key embedded in mksapkg-OS5 / upload_apkg (public
# knowledge; WD's own packages use the same scheme).
SIGN_KEY="Lidho.mdk3K3h"

# Release metadata this gate enforces (not just internal agreement between
# header and apkg.rc). Override per release: EXPECTED_VERSION=0.2.2 ...
EXPECTED_VERSION="${EXPECTED_VERSION:-0.2.4}"
EXPECTED_MINFW="${EXPECTED_MINFW:-5.27.100}"

BIN_PATH="${1:-}"
if [ -z "${BIN_PATH}" ]; then
	BIN_PATH="$(find packages -maxdepth 1 -name 'MyCloudPR4100_*.bin' | head -n 1)"
fi
[ -n "${BIN_PATH}" ] && [ -f "${BIN_PATH}" ] || { echo "FAIL: no .bin package found" >&2; exit 1; }

fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { printf 'ok: %s\n' "$*"; }

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# --- 1. header format -------------------------------------------------------
python3 - "${BIN_PATH}" "${WORK}/header.env" <<'PY'
import shlex
import sys
data = open(sys.argv[1], 'rb').read()
size = len(data)
if size < 1024 * 1024:
    sys.exit("FAIL: .bin is implausibly small (%d bytes)" % size)
if data[:7] != b'BlackCy':
    sys.exit("FAIL: bad header magic %r (expected BlackCy*)" % data[:8])
module = data[8:64].split(b'\0')[0].decode()
version = data[0x4C:0x4C+16].split(b'\0')[0].decode()
gz = data.find(b'\x1f\x8b')
if gz < 0 or gz > 4096:
    sys.exit("FAIL: no gzip payload found in the header region")
open(sys.argv[2], 'w').write('HDR_MODULE=%s\nHDR_VERSION=%s\nGZ_OFF=%d\nBIN_SIZE=%d\n' % (shlex.quote(module), shlex.quote(version), gz, size))
print("header: module=%s version=%s payload@%d size=%d" % (module, version, gz, size))
PY
. "${WORK}/header.env"
[ "${HDR_MODULE}" = "${MODULE}" ] || fail "header module '${HDR_MODULE}' != '${MODULE}'"
ok "header magic/module/version/size (${HDR_VERSION}, ${BIN_SIZE} bytes)"

# --- 2. extract payload -----------------------------------------------------
dd if="${BIN_PATH}" bs=1 skip="${GZ_OFF}" 2>/dev/null | tar xz -C "${WORK}" \
	|| fail "payload does not extract as tar.gz"
PAYLOAD_ROOT="$(find "${WORK}" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | head -n 1)"
PKG_DIR="${WORK}/${PAYLOAD_ROOT}"
[ -d "${PKG_DIR}" ] || fail "no payload directory"
ok "payload extracts"

# --- 3. payload root dir == module name -------------------------------------
[ "${PAYLOAD_ROOT}" = "${MODULE}" ] \
	|| fail "payload root dir '${PAYLOAD_ROOT}' != module name '${MODULE}' (device probes _install/<module>/apkg.sign)"
ok "payload root dir == module name"

# --- 4. apkg.rc sanity -------------------------------------------------------
RC="${PKG_DIR}/apkg.rc"
[ -f "${RC}" ] || fail "apkg.rc missing from payload"
rc_val() { awk -F: -v k="$1" '$1==k {sub(/^[ \t]+/,"",$2); sub(/[ \t]+$/,"",$2); print $2}' "${RC}"; }
for field in Package Version Packager Description Icon AddonShowName MinFWVer; do
	v="$(rc_val "${field}")"
	[ -n "${v}" ] || fail "apkg.rc field '${field}' missing or empty"
done
[ "$(rc_val Package)" = "${MODULE}" ] || fail "apkg.rc Package '$(rc_val Package)' != '${MODULE}'"
[ "$(rc_val Version)" = "${HDR_VERSION}" ] || fail "apkg.rc Version '$(rc_val Version)' != header version '${HDR_VERSION}'"
[ "${HDR_VERSION}" = "${EXPECTED_VERSION}" ] || fail "version '${HDR_VERSION}' != expected release '${EXPECTED_VERSION}'"
[ "$(rc_val MinFWVer)" = "${EXPECTED_MINFW}" ] || fail "MinFWVer '$(rc_val MinFWVer)' != expected '${EXPECTED_MINFW}'"
ICON="$(rc_val Icon)"
case "${ICON}" in
	*.png) ;;
	*) fail "apkg.rc Icon '${ICON}' is not a PNG (platform norm)" ;;
esac
[ -f "${PKG_DIR}/${ICON}" ] || fail "apkg.rc Icon '${ICON}' not found in payload"
ok "apkg.rc fields sane (Version ${HDR_VERSION}, MinFWVer $(rc_val MinFWVer), icon ${ICON})"

# --- 5. expected payload contents -------------------------------------------
for f in bin/wdf_demo models/scrfd_500m.param models/scrfd_500m.bin models/mobilefacenet.param models/mobilefacenet.bin; do
	[ -f "${PKG_DIR}/${f}" ] || fail "payload missing ${f}"
done
demo_out="$("${PKG_DIR}/bin/wdf_demo" --version 2>&1)" \
	|| fail "wdf_demo --version failed to execute (output: ${demo_out:-none})"
case "${demo_out}" in
	wdf_demo\ *) ;;
	*) fail "wdf_demo --version output unexpected: ${demo_out}" ;;
esac
ok "payload contents + wdf_demo smoke-run"

# --- 6. signature ------------------------------------------------------------
SIGN="${PKG_DIR}/apkg.sign"
[ -f "${SIGN}" ] || fail "apkg.sign missing from payload"
sign_size="$(wc -c < "${SIGN}")"
[ "${sign_size}" -ge 24 ] || fail "apkg.sign is ${sign_size} bytes - EMPTY signature (openssl lacked bf-cbc at build time?)"
[ $(( (sign_size - 16) % 8 )) -eq 0 ] || fail "apkg.sign size ${sign_size} is not a blowfish block multiple"
decrypt() { openssl enc -d -bf-cbc -md sha256 "$@" -in "${SIGN}" -k "${SIGN_KEY}" 2>/dev/null; }
plain="$(decrypt || true)"
if [ -z "${plain}" ]; then
	plain="$(decrypt -provider default -provider legacy || true)"
fi
[ -n "${plain}" ] || fail "apkg.sign does not decrypt (bf-cbc unavailable on this machine even via the legacy provider, or wrong key)"
[ "${plain}" = "${MODULE}" ] || fail "apkg.sign decrypts to '${plain}', expected '${MODULE}'"
ok "apkg.sign present (${sign_size} bytes) and decrypts to the module name"

echo
echo "PASS: ${BIN_PATH} is a well-formed, correctly signed OS 5 package."
