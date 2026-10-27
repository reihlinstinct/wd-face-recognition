#!/usr/bin/env bash
# Builds the wd-face-recognition OS 5 package (.bin) for the My Cloud PR4100.
# Runs inside a Debian 11 (Bullseye) environment (CI container or local Docker):
#  1. C++ worker, fully static (g++ -static)
#  2. Rust service, fully static (x86_64-unknown-linux-musl)
#  3. apkg .bin via WD's mksapkg-OS5 packager, pinned by commit + sha256
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL="${MODEL_OVERRIDE:-MyCloudPR4100}"

# This build produces x86-64 binaries only; reject models of other archs.
case "${MODEL}" in
	MyCloudPR4100|MyCloudPR2100|WDMyCloudDL4100|WDMyCloudDL2100) ;;
	*)
		echo "unsupported model '${MODEL}': this build produces x86-64 binaries only" >&2
		exit 1
		;;
esac

# WD's mksapkg-OS5 is no longer shipped by WD; the community copy comes from
# jessedp/WD-NAS-App-Builder, pinned to an exact commit and sha256.
MKSAPKG_COMMIT="252af116c6b8c03cc4aae046f079ba9213b15a9d"
MKSAPKG_SHA256="62340b1d0eb0433fffa7ceb1a5b6d4886e69b93e6e60297b2f984f5f3557e67a"
MKSAPKG_URL="https://raw.githubusercontent.com/jessedp/WD-NAS-App-Builder/${MKSAPKG_COMMIT}/mksapkg-OS5"

WORKER_BUILD="${REPO_ROOT}/build/worker"
SERVICE_TARGET_DIR="${REPO_ROOT}/build/service"
STAGE_BIN="${REPO_ROOT}/apkg/bin"
OUT_DIR="${REPO_ROOT}/packages"

log() { printf '\n=== %s ===\n' "$*"; }

require() { command -v "$1" >/dev/null 2>&1 || { echo "missing build tool: $1" >&2; exit 1; }; }
require cmake
require g++
require cargo
require curl

log "1/4 worker (C++, static)"
cmake -S "${REPO_ROOT}/worker" -B "${WORKER_BUILD}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${WORKER_BUILD}" --parallel
check_static() {
	# A dynamically linked ELF has an INTERP segment; a fully static one does not.
	if readelf -l "$1" | grep -q 'INTERP'; then
		echo "$1 is not statically linked" >&2
		exit 1
	fi
}
check_static "${WORKER_BUILD}/wdf-worker"

log "2/4 service (Rust, static musl)"
cargo build --release --target x86_64-unknown-linux-musl \
	--manifest-path "${REPO_ROOT}/service/Cargo.toml" \
	--target-dir "${SERVICE_TARGET_DIR}"
check_static "${SERVICE_TARGET_DIR}/x86_64-unknown-linux-musl/release/wdf-service"

log "3/4 staging binaries into apkg/bin"
mkdir -p "${STAGE_BIN}"
cp "${WORKER_BUILD}/wdf-worker" "${STAGE_BIN}/"
cp "${SERVICE_TARGET_DIR}/x86_64-unknown-linux-musl/release/wdf-service" "${STAGE_BIN}/"

log "4/4 mksapkg-OS5 (${MODEL})"
MKSAPKG="$(mktemp)"
curl -fsSL -o "${MKSAPKG}" "${MKSAPKG_URL}"
echo "${MKSAPKG_SHA256}  ${MKSAPKG}" | sha256sum --check
chmod +x "${MKSAPKG}"

VERSION="$(awk -F: '/^Version:/ {gsub(/^[ \t]+|[ \t]+$/, "", $2); print $2}' "${REPO_ROOT}/apkg/apkg.rc")"

# mksapkg writes the package to the PARENT of the app directory and appends a
# build-date suffix, e.g. MyCloudPR4100_<app>_<version>.bin(10272026).
# Remove stale packages first so only the one from THIS build can match.
rm -f "${REPO_ROOT}/${MODEL}_wd-face-recognition_"*.bin*

cd "${REPO_ROOT}/apkg"
"${MKSAPKG}" -E -s -m "${MODEL}" > /dev/null
rm -f "${MKSAPKG}"
rm -f apkg.sign apkg.xml

BIN_PATH="$(find "${REPO_ROOT}" -maxdepth 1 -name "${MODEL}_wd-face-recognition_${VERSION}.bin*" | head -n 1)"
if [ -z "${BIN_PATH}" ]; then
	echo "no .bin package produced for version ${VERSION}" >&2
	exit 1
fi
mkdir -p "${OUT_DIR}"
FINAL="${OUT_DIR}/${MODEL}_wd-face-recognition_${VERSION}.bin"
mv "${BIN_PATH}" "${FINAL}"
log "done: ${FINAL}"
