# Language shootout results (issue #14)

Generated: 2026-10-27T16:39:18+00:00

**Container ESTIMATES** - Debian 11 (Bullseye), cgroup-limited to 4 GB RAM + 2 CPUs,
AVX code paths disabled (baseline x86-64), synthetic assets only (bench/assets/).
The on-device self-test (docs/DEVICE-TEST.md) remains the authority.

Environment: Linux-7.0.0-1017-azure-x86_64-with-glibc2.31 | 4 visible CPUs | Python 3.9.2
Method: median of 5 repetitions, language order alternated each repetition;
peak RSS via GNU time; one shared C++ HTTP client (one connection
per request; 5 x 2000 requests, median) drives every server.

| Workload | C++ | Rust | Python | Peak RSS (MiB, C++/Rust/Python) | Cold start (s, C++/Rust/Python) |
| --- | --- | --- | --- | --- | --- |
| 1. SHA-256, 12 MP file stream (MB/s) | 1792.83 | 1791.38 | 1773.95 | 7.5/2.7/20.2 | 0.035/0.033/0.080 |
| 2. EXIF parse, synthetic JPEG (parses/s) | 27833.18 | 146042.33 | 14544.37 | 7.6/3.0/22.0 | 0.016/0.005/0.072 |
| 3. JPEG decode, 12 MP photo (photos/s) | 12.30 | 10.75 | 10.12 | 7.6/47.7/67.3 | 0.247/0.277/0.350 |
| 4. HTTP + JSON local API (req/s) | 11180.91 | 10969.75 | 5937.81 | 7.6/3.1/20.7 | 0.012/0.011/0.041 |
| 5. SQLite batch insert, 20k faces (rows/s) | 165404.72 | 154174.85 | 138534.64 | 9.8/6.4/22.6 | 0.194/0.123/0.202 |

## Notes

- Workload 6 (ncnn inference FFI overhead) is measured in the ncnn spike
  (issue #3), which builds ncnn anyway; its numbers get appended here.
- Go skipped per the owner decision (reference only if trivial; it is not).
- Realistic library paths: C++ OpenSSL/Exiv2/libjpeg-turbo/httplib+nlohmann-json/sqlite3;
  Rust sha2/kamadak-exif/mozjpeg/tiny_http+serde_json/rusqlite(bundled);
  Python hashlib/Pillow(PIL)/http.server+json/sqlite3.
- Cold start for HTTP servers is time-to-first-200 on /ready; for CLI
  workloads it is each language's first process run (a process-start
  sample; the file cache may already be warm for languages measured
  later in that repetition).

## ADR-001 impact

Filled in after review of these numbers against the DESIGN.md section 2a
performance budgets. (Recorded in issue #14.)
