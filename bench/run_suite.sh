#!/usr/bin/env bash
# Thin wrapper: runs the whole language shootout (issue #14).
# Expects bench_cpp and bench-rust already built (see .github/workflows/bench.yml).
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASSETS="${1:-/tmp/wdf-bench-assets}"
CPP_BIN="${2:-${REPO_ROOT}/build/bench-cpp/bench_cpp}"
RUST_BIN="${3:-${REPO_ROOT}/build/bench-rust/release/wdf-bench-rust}"
PY="${4:-python3}"
python3 "${REPO_ROOT}/bench/assets/gen_assets.py" "${ASSETS}"
exec python3 "${REPO_ROOT}/bench/run_suite.py" "${ASSETS}" "${CPP_BIN}" "${RUST_BIN}" "${PY}"
