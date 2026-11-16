#!/usr/bin/env bash
# Fetches three public-domain portrait photos (Wikimedia Commons, pinned
# URL + sha256) for the demo smoke test: real faces, so detection,
# embedding, clustering and the HTML output are all exercised end to end.
set -euo pipefail

OUT="${1:-/tmp/wdf-photos}"
mkdir -p "${OUT}"
cd "${OUT}"

fetch() { # <url> <file> <sha256>
	if [ ! -f "$2" ]; then
		curl -fsSL -o "$2" "$1"
	fi
	echo "$3  $2" | sha256sum --check
}

fetch "https://upload.wikimedia.org/wikipedia/commons/a/ab/Abraham_Lincoln_O-77_matte_collodion_print.jpg" \
	lincoln.jpg f5e79a35f74b54e0435dfc87f5956d6917725e2f5b8c17932ecf5bad704e29b2
fetch "https://upload.wikimedia.org/wikipedia/commons/a/ad/Commodore_Grace_M._Hopper%2C_USN_%28covered%29.jpg" \
	hopper.jpg 198babb1067fc623a57ec8955b5b7025ff2fb652cedb2d21eec65b8733a89cb4
fetch "https://upload.wikimedia.org/wikipedia/commons/3/3e/Einstein_1921_by_F_Schmutzer_-_restoration.jpg" \
	einstein.jpg 95b532a34e4446651f1151d836f0eda9b3e7a9dff6ac368073b8cd887b04dd8c
ls -la
