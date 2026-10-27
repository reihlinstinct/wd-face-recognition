#!/usr/bin/env bash
# Builds ncnn (pinned tag + sha256) as a static library with ALL AVX-family
# code paths OFF (baseline x86-64, matching the PR4100's N3710: SSE4.2 max),
# plus the onnx2ncnn tool. Idempotent: skips the build if the install prefix
# already exists (CI caches it).
set -euo pipefail

NCNN_TAG="20261020"
NCNN_SHA256="968d8d8e65ccadc803322f8238736176e0f9c929fcf484895f58a63e6b227127"
NCNN_URL="https://github.com/Tencent/ncnn/archive/refs/tags/${NCNN_TAG}.tar.gz"

WORK="${1:-/tmp/ncnn-build}"
PREFIX="${WORK}/install"
SRC="${WORK}/ncnn-${NCNN_TAG}"

if [ -f "${PREFIX}/lib/libncnn.a" ] && [ -x "${PREFIX}/bin/onnx2ncnn" ]; then
	echo "ncnn ${NCNN_TAG} already built in ${PREFIX} - skipping"
	exit 0
fi

mkdir -p "${WORK}"
cd "${WORK}"
if [ ! -d "${SRC}" ]; then
	curl -fsSL -o ncnn.tar.gz "${NCNN_URL}"
	echo "${NCNN_SHA256}  ncnn.tar.gz" | sha256sum --check
	tar xzf ncnn.tar.gz
fi

# Pinned 20261020 omits add_subdirectory(onnx) from tools/CMakeLists.txt, so
# NCNN_BUILD_TOOLS=ON never builds onnx2ncnn. Patch it in (idempotent).
if ! grep -q 'add_subdirectory(onnx)' "${SRC}/tools/CMakeLists.txt"; then
	echo 'add_subdirectory(onnx)' >> "${SRC}/tools/CMakeLists.txt"
fi

cmake -S "${SRC}" -B "${WORK}/build" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="${PREFIX}" \
	-DNCNN_SHARED_LIB=OFF -DNCNN_BUILD_SHARED_LIBS=OFF \
	-DNCNN_BUILD_TOOLS=ON -DNCNN_BUILD_EXAMPLES=OFF \
	-DNCNN_BUILD_BENCHMARK=OFF -DNCNN_BUILD_TESTS=OFF \
	-DNCNN_VULKAN=OFF -DNCNN_OPENMP=ON \
	-DNCNN_AVX=OFF -DNCNN_AVX2=OFF -DNCNN_AVX512=OFF \
	-DNCNN_AVX512VNNI=OFF -DNCNN_AVXVNNI=OFF -DNCNN_AVX512BF16=OFF \
	-DNCNN_FMA=OFF -DNCNN_F16C=OFF -DNCNN_XOP=OFF
cmake --build "${WORK}/build" --parallel "$(nproc)"
cmake --install "${WORK}/build"
# onnx2ncnn is built but NOT covered by cmake --install; copy it ourselves.
TOOL="$(find "${WORK}/build" -name onnx2ncnn -type f | head -n 1)"
if [ -z "${TOOL}" ]; then
	echo "onnx2ncnn was not built (protobuf missing?)" >&2
	exit 1
fi
mkdir -p "${PREFIX}/bin"
cp "${TOOL}" "${PREFIX}/bin/onnx2ncnn"
echo "ncnn ${NCNN_TAG} (no-AVX static) installed to ${PREFIX}"
