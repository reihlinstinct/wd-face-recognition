# Benchmarks

M2 benchmark spikes (docs/DESIGN.md milestone 2; issues #3 and #14).

- `assets/gen_assets.py` - synthetic test assets (12 MP JPEG, EXIF JPEG). No real photos.
- `cpp/`, `rust/`, `python/` - identical workloads in each runtime (issue #14).
- `run_suite.sh` - builds nothing; runs the whole shootout and writes
  `docs/BENCHMARK-LANGUAGE.md` (median of N runs, ops/sec + peak RSS + cold start).

Everything runs in the Debian 11 (Bullseye) CI container, cgroup-limited to
4 GB RAM and 2 CPUs to approximate the PR4100. AVX code paths are off.
Container numbers are ESTIMATES - the on-device self-test
(docs/DEVICE-TEST.md) stays the authority.

## Workloads (issue #14)

1. SHA-256 over a 12 MP file stream
2. EXIF parse of a synthetic JPEG
3. JPEG decode via each runtime's realistic libjpeg-turbo-class path
4. HTTP + JSON request handling (local-API shape)
5. SQLite batch writes
6. ncnn inference FFI overhead - deferred to the ncnn spike (issue #3),
   which builds ncnn anyway; results will be appended to the same doc.

Go is a reference candidate per the owner decision and is skipped unless its
inclusion is trivial; the shootout compares C++ vs Rust vs Python.
