#!/usr/bin/env python3
"""wdf-bench-py: language shootout, Python side (issue #14).

Subcommands mirror the C++/Rust benches:
  sha256 <file> [n] | exif <file> [n] | jpeg <file> [n]
  http-server <port> | sqlite <dir> [rows]
Realistic paths: hashlib (OpenSSL), PIL.Exif, Pillow decode (libjpeg-turbo),
http.server + json (stdlib), sqlite3 (stdlib).
"""
import hashlib
import http.server
import json
import os
import sqlite3
import sys
import time

from PIL import Image

WORK_RESPONSE = b'{"ok":true,"faces":3,"elapsed_ms":42}'


def workload_sha256(path, iterations):
    t0 = time.perf_counter()
    total = 0
    for it in range(iterations):
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 16), b""):
                h.update(chunk)
                total += len(chunk)
        if it == 0:
            print(f"digest0={h.digest()[:4].hex()}")
    secs = time.perf_counter() - t0
    print(f"mb_per_sec={total / 1e6 / secs:.2f}")


def workload_exif(path, iterations):
    t0 = time.perf_counter()
    for it in range(iterations):
        with Image.open(path) as img:
            exif = img.getexif()
            if it == 0:
                print(f"make={exif.get(0x010F)}")
    secs = time.perf_counter() - t0
    print(f"ops_per_sec={iterations / secs:.2f}")


def workload_jpeg(path, iterations):
    pixels = 0
    t0 = time.perf_counter()
    for _ in range(iterations):
        with Image.open(path) as img:
            img.load()
            pixels += img.width * img.height
    secs = time.perf_counter() - t0
    print(f"photos_per_sec={iterations / secs:.3f} mpix_per_sec={pixels / 1e6 / secs:.2f}")


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/ready":
            body = b"ready"
        else:
            body = WORK_RESPONSE
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        payload = self.rfile.read(length)
        json.loads(payload)  # realistic local-API shape: parse, then answer
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(WORK_RESPONSE)))
        self.end_headers()
        self.wfile.write(WORK_RESPONSE)


def workload_http_server(port):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), _Handler)
    print(f"listening on {port}", file=sys.stderr, flush=True)
    server.serve_forever()


def workload_sqlite(directory, rows):
    db_path = os.path.join(directory, "bench_py.sqlite")
    if os.path.exists(db_path):
        os.remove(db_path)
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    conn.execute(
        "CREATE TABLE faces (id INTEGER PRIMARY KEY, photo TEXT, x REAL, y REAL, embedding BLOB)"
    )
    t0 = time.perf_counter()
    conn.executemany(
        "INSERT INTO faces (photo, x, y, embedding) VALUES (?, ?, ?, zeroblob(512))",
        (("photo_12mp.jpg", 0.1 * (i % 100), 0.2 * (i % 50)) for i in range(rows)),
    )
    conn.commit()
    secs = time.perf_counter() - t0
    conn.close()
    os.remove(db_path)
    print(f"rows_per_sec={rows / secs:.2f}")


def main():
    cmd = sys.argv[1]
    iterations = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    if cmd == "sha256":
        workload_sha256(sys.argv[2], iterations)
    elif cmd == "exif":
        workload_exif(sys.argv[2], iterations)
    elif cmd == "jpeg":
        workload_jpeg(sys.argv[2], iterations)
    elif cmd == "http-server":
        workload_http_server(int(sys.argv[2]))
    elif cmd == "sqlite":
        workload_sqlite(sys.argv[2], iterations)
    else:
        sys.exit(f"unknown workload: {cmd}")


if __name__ == "__main__":
    main()
