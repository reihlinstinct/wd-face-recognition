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
if [ ! -f buffalo_s.zip ]; then
	curl -fsSL -o buffalo_s.zip "${BUFFALO_S_URL}"
fi
echo "${BUFFALO_S_SHA256}  buffalo_s.zip" | sha256sum --check
unzip -o -q buffalo_s.zip det_500m.onnx w600k_mbf.onnx

# det_500m.onnx has dynamic H/W with Shape/Gather/Unsqueeze/Resize chains that
# onnx2ncnn cannot convert; fold them away for the fixed 640x640 benchmark input.
python3 -m onnxsim det_500m.onnx det_500m_sim.onnx --input-shape 1,3,640,640
"${NCNN_PREFIX}/bin/onnx2ncnn" det_500m_sim.onnx scrfd_500m.param scrfd_500m.bin
"${NCNN_PREFIX}/bin/onnx2ncnn" w600k_mbf.onnx mobilefacenet.param mobilefacenet.bin
ls -la scrfd_500m.param scrfd_500m.bin mobilefacenet.param mobilefacenet.bin
