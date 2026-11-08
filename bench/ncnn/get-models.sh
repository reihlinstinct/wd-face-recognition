#!/usr/bin/env bash
# Fetches the insightface buffalo_s pack (pinned URL + sha256) and converts
# det_500m.onnx (SCRFD-500M detection) and w600k_mbf.onnx (MobileFaceNet
# ArcFace embedding) to ncnn format with the onnx2ncnn tool from build-ncnn.sh.
set -euo pipefail

BUFFALO_S_URL="https://github.com/deepinsight/insightface/releases/download/model-zoo/buffalo_s.zip"
BUFFALO_S_SHA256="d85a87f503f691807cd8bb97128bdf7a0660326cd9cd02657127fa978bab8b5e"

OUT="${1:-/tmp/wdf-models}"
NCNN_PREFIX="${2:-/tmp/ncnn-build/install}"

mkdir -p "${OUT}"
cd "${OUT}"
fetch() {
	# Retry transient gateway/server errors: release-assets.githubusercontent
	# held the connection ~120s AFTER connect and then 504'd on five
	# package runs (2026-11-08). --connect-timeout does not cover that, so
	# --max-time bounds every attempt (curl retries a timeout as
	# transient). The pinned sha256 below guards integrity; retries only
	# ride over transient transport failures. The package workflow also
	# caches this zip, so the download normally happens once.
	curl -fsSL --retry 5 --retry-delay 10 --retry-all-errors \
		--connect-timeout 30 --max-time 300 -o buffalo_s.zip "${BUFFALO_S_URL}"
}
if [ ! -f buffalo_s.zip ]; then
	fetch
fi
# The pinned sha256 is checked on every run, cache hit or not. A cached
# zip that fails the checksum is deleted and re-downloaded once: a bad
# cache entry must not wedge every run with this key (Tab, PR #152).
if ! echo "${BUFFALO_S_SHA256}  buffalo_s.zip" | sha256sum --check; then
	echo "cached buffalo_s.zip failed sha256; re-downloading" >&2
	rm -f buffalo_s.zip
	fetch
	echo "${BUFFALO_S_SHA256}  buffalo_s.zip" | sha256sum --check
fi
unzip -o -q buffalo_s.zip det_500m.onnx w600k_mbf.onnx

# det_500m.onnx has dynamic H/W with Shape/Gather/Unsqueeze/Resize chains that
# onnx2ncnn cannot convert; fold them away for the fixed 640x640 benchmark input.
python3 -m onnxsim det_500m.onnx det_500m_sim.onnx --input-shape 1,3,640,640
"${NCNN_PREFIX}/bin/onnx2ncnn" det_500m_sim.onnx scrfd_500m.param scrfd_500m.bin
"${NCNN_PREFIX}/bin/onnx2ncnn" w600k_mbf.onnx mobilefacenet.param mobilefacenet.bin
ls -la scrfd_500m.param scrfd_500m.bin mobilefacenet.param mobilefacenet.bin
