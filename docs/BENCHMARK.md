# M2 benchmark: ncnn pipeline spike (issue #3)

Generated: 2026-10-27T16:23:22+00:00

**Container ESTIMATES** - Debian 11 (Bullseye), cgroup-limited to 4 GB RAM + 2 CPUs,
ncnn built STATIC with every AVX-family code path OFF (baseline x86-64, SSE2 only),
approximating the PR4100's Pentium N3710 (SSE4.2, no AVX). Synthetic assets only.
The on-device self-test (docs/DEVICE-TEST.md) remains the authority for go/no-go.

Models: SCRFD-500M (det_500m) + MobileFaceNet ArcFace (w600k_mbf) from the
insightface buffalo_s pack (pinned URL + sha256), simplified with onnxsim and converted with pnnx.

| Stage | decode ms | detect ms | embed ms | total ms | faces |
| --- | --- | --- | --- | --- | --- |
| Decode + detect 12 MP (embed only when faces > 0), 1 thread(s) | 114.5 | 50.3 | 0.0 | 165.5 | 0 |
| Decode + detect 12 MP (embed only when faces > 0), 2 thread(s) | 115.6 | 29.5 | 0.0 | 146.8 | 0 |
| Per-face embedding only, 1 thread | - | - | 33.1 | - | - |
| HEIC decode, 12 MP | 939.5 | - | - | - | - |

| Configuration | Peak RSS (MiB) |
| --- | --- |
| pipeline 1t | 100.3 |
| pipeline 2t | 100.4 |
| embed | 30.2 |
| heic | 119.7 |

## Assessment vs DESIGN.md section 3 targets

- Target: ~0.5-2 s/photo detection, <= 1.5 GB RSS.
- The synthetic 12 MP photo has no real faces; detection latency is
  content-independent (fixed 640x640 SCRFD input), so the numbers hold.
  Embedding latency uses a clamped 112x112 crop; alignment arrives with
  the indexer and changes cost by <1 ms.
- HEIC: libheif decode works in the Bullseye container.
- Go/no-go decision: recorded in issue #3 after review of these numbers.
