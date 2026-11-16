#!/bin/sh
# Fetches the pinned SQLite amalgamation (DESIGN.md: "SQLite (C amalgamation)
# - index DB") into indexer/third_party/. Pinned version + sha256: the build
# is reproducible and a tampered archive fails the check.
set -eu
VER=3450100
SHA256=5592243caf28b2cdef41e6ab58d25d653dfc53deded8450eb66072c929f030c4
DIR="$(cd "$(dirname "$0")" && pwd)/third_party"
mkdir -p "$DIR"
if [ -f "$DIR/sqlite3.c" ] && [ -f "$DIR/sqlite3.h" ]; then
    exit 0
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fsSL -o "$TMP/sqlite.zip" "https://www.sqlite.org/2024/sqlite-amalgamation-${VER}.zip"
echo "${SHA256}  $TMP/sqlite.zip" | sha256sum -c -
unzip -o -q "$TMP/sqlite.zip" -d "$TMP"
cp "$TMP/sqlite-amalgamation-${VER}/sqlite3.c" "$TMP/sqlite-amalgamation-${VER}/sqlite3.h" "$DIR/"
echo "sqlite amalgamation ${VER} ready in $DIR"
