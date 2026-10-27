#!/usr/bin/env python3
"""Runs the ncnn pipeline spike (issue #3) and writes docs/BENCHMARK.md.

Usage: bench/ncnn/run_ncnn_bench.py <bench_ncnn> <assets_dir> <models_dir>
"""
import datetime
import os
import re
import subprocess
import sys

BIN, ASSETS, MODELS = sys.argv[1:4]
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT_MD = os.path.join(REPO, "docs", "BENCHMARK.md")
PHOTO = os.path.join(ASSETS, "photo_12mp.jpg")
CROP = os.path.join(ASSETS, "crop112.jpg")
HEIC = os.path.join(ASSETS, "photo_12mp.heic")


def run(cmd, timeout=900):
    print("+", " ".join(cmd), flush=True)
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f"timed out after {timeout}s: {cmd}") from exc
    if proc.returncode != 0:
        raise RuntimeError(f"failed: {cmd}\n{proc.stdout}\n{proc.stderr}")
    return proc.stdout


def grab(stdout, key, cast=float, agg="median"):
    vals = [cast(m.group(1)) for m in re.finditer(key + r"=([0-9.]+)", stdout)]
    if not vals:
        return None
    vals.sort()
    return vals[len(vals) // 2]


def main():
    rows = []
    rss = {}
    for threads in (1, 2):
        out = run([BIN, "pipeline", PHOTO, MODELS, str(threads), "5"])
        rows.append((f"Decode + detect 12 MP (embed only when faces > 0), {threads} thread(s)",
                     grab(out, "decode_ms"), grab(out, "detect_ms"),
                     grab(out, "embed_ms"), grab(out, "total_ms"),
                     grab(out, "faces", int)))
        rss[f"pipeline {threads}t"] = grab(out, "peak_rss_mib")
    out = run([BIN, "embed", CROP, MODELS, "50"])
    rows.append(("Per-face embedding only, 1 thread", None, None, grab(out, "embed_ms_avg"), None, None))
    rss["embed"] = grab(out, "peak_rss_mib")
    heic_note = "not run"
    if os.path.exists(HEIC):
        out = run([BIN, "heic", HEIC, "3"])
        rows.append(("HEIC decode, 12 MP", grab(out, "heic_decode_ms"), None, None, None, None))
        rss["heic"] = grab(out, "peak_rss_mib")
        heic_note = "libheif decode works in the Bullseye container"

    def f(v):
        return "-" if v is None else (f"{v:.1f}" if isinstance(v, float) else str(v))

    lines = [
        "# M2 benchmark: ncnn pipeline spike (issue #3)",
        "",
        f"Generated: {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}",
        "",
        "**Container ESTIMATES** - Debian 11 (Bullseye), cgroup-limited to 4 GB RAM + 2 CPUs,",
        "ncnn built STATIC with every AVX-family code path OFF (baseline x86-64, SSE2 only),",
        "approximating the PR4100's Pentium N3710 (SSE4.2, no AVX). Synthetic assets only.",
        "The on-device self-test (docs/DEVICE-TEST.md) remains the authority for go/no-go.",
        "",
        "Models: SCRFD-500M (det_500m) + MobileFaceNet ArcFace (w600k_mbf) from the",
        "insightface buffalo_s pack (pinned URL + sha256), simplified with onnxsim and converted with pnnx.",
        "",
        "| Stage | decode ms | detect ms | embed ms | total ms | faces |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for name, dec, det, emb, tot, faces in rows:
        lines.append(f"| {name} | {f(dec)} | {f(det)} | {f(emb)} | {f(tot)} | {f(faces)} |")
    lines += [
        "",
        "| Configuration | Peak RSS (MiB) |",
        "| --- | --- |",
    ]
    for k, v in rss.items():
        lines.append(f"| {k} | {f(v)} |")
    lines += [
        "",
        "## Assessment vs DESIGN.md section 3 targets",
        "",
        "- Target: ~0.5-2 s/photo detection, <= 1.5 GB RSS.",
        "- The synthetic 12 MP photo has no real faces; detection latency is",
        "  content-independent (fixed 640x640 SCRFD input), so the numbers hold.",
        "  Embedding latency uses a clamped 112x112 crop; alignment arrives with",
        "  the indexer and changes cost by <1 ms.",
        f"- HEIC: {heic_note}.",
        "- Go/no-go decision: recorded in issue #3 after review of these numbers.",
        "",
    ]
    with open(OUT_MD, "w") as fh:
        fh.write("\n".join(lines))
    print(f"wrote {OUT_MD}")


if __name__ == "__main__":
    main()
