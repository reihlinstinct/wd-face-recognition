#!/usr/bin/env python3
"""Runs the language shootout (issue #14) and writes docs/BENCHMARK-LANGUAGE.md.

Usage: bench/run_suite.py <assets_dir> <cpp_bin> <rust_bin> <python_exe>
Medians over REPS repetitions; peak RSS via GNU time; http via the shared
C++ client, one connection per request, so server cost dominates.
"""
import datetime
import http.client
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import time

ASSETS, CPP_BIN, RUST_BIN, PY = sys.argv[1:5]
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_MD = os.path.join(REPO, "docs", "BENCHMARK-LANGUAGE.md")
REPS = 5
HTTP_REQUESTS = 2000
HTTP_PORT_BASE = 8471

PHOTO = os.path.join(ASSETS, "photo_12mp.jpg")
EXIF_JPG = os.path.join(ASSETS, "exif.jpg")


def run_timed(cmd, timeout=300):
    """One repetition: returns (stdout, wall_secs, peak_rss_kib)."""
    with tempfile.NamedTemporaryFile("r", suffix=".time", delete=False) as tf:
        time_out = tf.name
    try:
        start = time.perf_counter()
        try:
            proc = subprocess.run(
                ["/usr/bin/time", "-v", "-o", time_out, *cmd],
                capture_output=True, text=True, timeout=timeout,
            )
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(f"{cmd} timed out after {timeout}s") from exc
        wall = time.perf_counter() - start
        with open(time_out) as f:
            stats = f.read()
        if proc.returncode != 0:
            raise RuntimeError(f"{cmd} failed:\n{proc.stdout}\n{proc.stderr}\n{stats}")
        m = re.search(r"Maximum resident set size \(kbytes\): (\d+)", stats)
        return proc.stdout, wall, int(m.group(1)) if m else 0
    finally:
        if os.path.exists(time_out):
            os.unlink(time_out)


def metrics(stdout):
    out = {}
    for line in stdout.splitlines():
        for k, v in re.findall(r"([A-Za-z0-9_]+)=([^\s]+)", line):
            try:
                out[k] = float(v)
            except ValueError:
                pass
    return out


# Executed language order per workload (retained with the raw run log).
ORDER_LOG = {}


def bench_cli(name, commands, primary_key):
    """commands: {lang: argv}; returns {lang: {metric, rss_kib, cold_secs}}.

    Alternates language order every repetition so cache/host drift over the
    suite does not systematically favour whichever language runs first; the
    executed order is returned under the "_order" key for the report.
    """
    raw = {lang: {"values": [], "rss_kib": 0, "cold_secs": None} for lang in commands}
    base = list(commands)
    order = []
    for rep in range(REPS + 1):
        langs = base if rep % 2 == 0 else base[::-1]
        order.append(list(langs))
        for lang in langs:
            stdout, wall, peak = run_timed(commands[lang])
            if rep == 0:  # first run doubles as the process-start (cold) sample
                raw[lang]["cold_secs"] = wall
            else:
                raw[lang]["values"].append(metrics(stdout)[primary_key])
            raw[lang]["rss_kib"] = max(raw[lang]["rss_kib"], peak)
    ORDER_LOG[name] = order
    return {
        lang: {
            "metric": statistics.median(r["values"]),
            "rss_kib": r["rss_kib"],
            "cold_secs": r["cold_secs"],
        }
        for lang, r in raw.items()
    }


def wait_ready(port, timeout=30.0):
    start = time.perf_counter()
    while True:
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
            conn.request("GET", "/ready")
            if conn.getresponse().status == 200:
                conn.close()
                return time.perf_counter() - start
            conn.close()
        except OSError:
            pass
        if time.perf_counter() - start > timeout:
            raise RuntimeError(f"server on :{port} never became ready")
        time.sleep(0.01)


def bench_http(servers):
    """servers: {lang: argv}; shared C++ client drives all of them.

    All servers run for the whole comparison; each repetition visits every
    server in alternating order (recorded in ORDER_LOG) so host drift does
    not systematically favour the first server measured.
    """
    port = HTTP_PORT_BASE
    procs, startup = {}, {}
    samples = {lang: [] for lang in servers}
    rss_kib = {lang: 0 for lang in servers}
    try:
        for lang, argv in servers.items():
            proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            procs[lang] = (proc, port)
            startup[lang] = wait_ready(port)
            port += 1
        base = list(servers)
        order = []
        for rep in range(REPS):
            langs = base if rep % 2 == 0 else base[::-1]
            order.append(list(langs))
            for lang in langs:
                proc, sport = procs[lang]
                stdout, _, _ = run_timed([CPP_BIN, "http-client", str(sport), str(HTTP_REQUESTS)])
                samples[lang].append(metrics(stdout)["req_per_sec"])
                with open(f"/proc/{proc.pid}/status") as f:
                    m = re.search(r"VmHWM:\s+(\d+) kB", f.read())
                    if m:
                        rss_kib[lang] = max(rss_kib[lang], int(m.group(1)))
    finally:
        for proc, _ in procs.values():
            proc.kill()
            proc.wait()
    ORDER_LOG["http"] = order
    return {
        lang: {
            "metric": statistics.median(samples[lang]),
            "rss_kib": rss_kib[lang],
            "cold_secs": startup[lang],
        }
        for lang in servers
    }


def fmt(v, digits=2):
    return f"{v:.{digits}f}"


def main():
    tmp = tempfile.mkdtemp(prefix="wdf-bench-")
    benches = {}

    benches["1. SHA-256, 12 MP file stream (MB/s)"] = (
        bench_cli(
            "sha256",
            {
                "C++": [CPP_BIN, "sha256", PHOTO, "5"],
                "Rust": [RUST_BIN, "sha256", PHOTO, "5"],
                "Python": [PY, os.path.join(REPO, "bench/python/bench_py.py"), "sha256", PHOTO, "5"],
            },
            "mb_per_sec",
        ),
        "MB/s",
    )
    benches["2. EXIF parse, synthetic JPEG (parses/s)"] = (
        bench_cli(
            "exif",
            {
                "C++": [CPP_BIN, "exif", EXIF_JPG, "300"],
                "Rust": [RUST_BIN, "exif", EXIF_JPG, "300"],
                "Python": [PY, os.path.join(REPO, "bench/python/bench_py.py"), "exif", EXIF_JPG, "300"],
            },
            "ops_per_sec",
        ),
        "parses/s",
    )
    benches["3. JPEG decode, 12 MP photo (photos/s)"] = (
        bench_cli(
            "jpeg",
            {
                "C++": [CPP_BIN, "jpeg", PHOTO, "3"],
                "Rust": [RUST_BIN, "jpeg", PHOTO, "3"],
                "Python": [PY, os.path.join(REPO, "bench/python/bench_py.py"), "jpeg", PHOTO, "3"],
            },
            "photos_per_sec",
        ),
        "photos/s",
    )
    benches["4. HTTP + JSON local API (req/s)"] = (
        bench_http(
            {
                "C++": [CPP_BIN, "http-server", str(HTTP_PORT_BASE)],
                "Rust": [RUST_BIN, "http-server", str(HTTP_PORT_BASE + 1)],
                "Python": [PY, os.path.join(REPO, "bench/python/bench_py.py"), "http-server", str(HTTP_PORT_BASE + 2)],
            }
        ),
        "req/s",
    )
    benches["5. SQLite batch insert, 20k faces (rows/s)"] = (
        bench_cli(
            "sqlite",
            {
                "C++": [CPP_BIN, "sqlite", tmp, "20000"],
                "Rust": [RUST_BIN, "sqlite", tmp, "20000"],
                "Python": [PY, os.path.join(REPO, "bench/python/bench_py.py"), "sqlite", tmp, "20000"],
            },
            "rows_per_sec",
        ),
        "rows/s",
    )

    lines = [
        "# Language shootout results (issue #14)",
        "",
        f"Generated: {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}",
        "",
        "**Container ESTIMATES** - Debian 11 (Bullseye), cgroup-limited to 4 GB RAM + 2 CPUs,",
        "AVX code paths disabled (baseline x86-64), synthetic assets only (bench/assets/).",
        "The on-device self-test (docs/DEVICE-TEST.md) remains the authority.",
        "",
        f"Environment: {platform.platform()} | {os.cpu_count()} visible CPUs | Python {platform.python_version()}",
        f"Method: median of {REPS} repetitions, language order alternated each repetition;",
        "peak RSS via GNU time; one shared C++ HTTP client (one connection",
        f"per request; {REPS} x {HTTP_REQUESTS} requests, median) drives every server.",
        "",
        "| Workload | C++ | Rust | Python | Peak RSS (MiB, C++/Rust/Python) | Cold start (s, C++/Rust/Python) |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for name, (result, unit) in benches.items():
        cells = [fmt(result[lang]["metric"]) for lang in ("C++", "Rust", "Python")]
        rss = "/".join(fmt(result[lang]["rss_kib"] / 1024, 1) for lang in ("C++", "Rust", "Python"))
        cold = "/".join(fmt(result[lang]["cold_secs"], 3) for lang in ("C++", "Rust", "Python"))
        lines.append(f"| {name} | {cells[0]} | {cells[1]} | {cells[2]} | {rss} | {cold} |")
    lines += [
        "",
        "## Notes",
        "",
        "- Workload 6 (ncnn inference FFI overhead) is measured in the ncnn spike",
        "  (issue #3), which builds ncnn anyway; its numbers get appended here.",
        "- Go skipped per the owner decision (reference only if trivial; it is not).",
        "- Realistic library paths: C++ OpenSSL/Exiv2/libjpeg-turbo/httplib+nlohmann-json/sqlite3;",
        "  Rust sha2/kamadak-exif/mozjpeg/tiny_http+serde_json/rusqlite(bundled);",
        "  Python hashlib/Pillow(PIL)/http.server+json/sqlite3.",
        "- Cold start for HTTP servers is time-to-first-200 on /ready; for CLI",
        "  workloads it is each language's first process run (a process-start",
        "  sample; the file cache may already be warm for languages measured",
        "  later in that repetition).",
        "",
        "## ADR-001 impact",
        "",
        "Filled in after review of these numbers against the DESIGN.md section 2a",
        "performance budgets. (Recorded in issue #14.)",
        "",
    ]
    os.makedirs(os.path.dirname(OUT_MD), exist_ok=True)
    with open(OUT_MD, "w") as f:
        f.write("\n".join(lines))
    print(f"wrote {OUT_MD}")
    print("executed language order per workload:")
    print(json.dumps(ORDER_LOG))
    print(json.dumps({name: {lang: res[lang]["metric"] for lang in res} for name, (res, _) in benches.items()}, indent=1))


if __name__ == "__main__":
    main()
