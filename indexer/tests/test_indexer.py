#!/usr/bin/env python3
"""End-to-end test for the wdf_indexer binary (issue #40): fixture tree,
two scans, mtime change, deletion - verified via stdout summaries and the
SQLite DB itself. Run: python3 indexer/tests/test_indexer.py /path/to/wdf_indexer
"""
import os
import sqlite3
import subprocess
import sys
import tempfile

def run(bin_path, conf):
    r = subprocess.run([bin_path, "scan", "--config", conf],
                       capture_output=True, text=True)
    assert r.returncode == 0, f"scan failed: {r.stderr}"
    return r.stdout

def main():
    bin_path = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="wfi-e2e-")
    root = os.path.join(tmp, "photos")
    data = os.path.join(tmp, "data")
    os.makedirs(os.path.join(root, "sub"))
    os.makedirs(data)
    for rel in ("a.jpg", "sub/b.jpg"):
        with open(os.path.join(root, rel), "w") as f:
            f.write("jpeg-bytes")
    with open(os.path.join(root, "notes.txt"), "w") as f:
        f.write("not a photo")
    conf = os.path.join(tmp, "wfi.conf")
    with open(conf, "w") as f:
        f.write(f"roots={root}\ndata_dir={data}\n")

    out1 = run(bin_path, conf)
    assert "new=2" in out1 and "skipped=0" in out1, out1
    out2 = run(bin_path, conf)
    assert "new=0" in out2 and "skipped=2" in out2, out2

    db = sqlite3.connect(os.path.join(data, "index.db"))
    rows = db.execute(
        "SELECT path, state, detector_ver FROM photos ORDER BY path").fetchall()
    assert len(rows) == 2 and all(r[1] == "queued" for r in rows), rows
    assert all(r[2] == "scrfd-500m-int8" for r in rows), rows
    jobs = db.execute("SELECT scanned, added, skipped FROM scan_jobs").fetchall()
    assert jobs[0][:3] == (2, 2, 0), jobs
    assert jobs[1][:3] == (2, 0, 2), jobs

    # mtime change on one file -> exactly one re-queued
    a = os.path.join(root, "a.jpg")
    st = os.stat(a)
    os.utime(a, (st.st_atime, st.st_mtime + 100))
    out3 = run(bin_path, conf)
    assert "changed=1" in out3, out3

    # deletion -> missing; file count stays
    os.unlink(os.path.join(root, "sub", "b.jpg"))
    out4 = run(bin_path, conf)
    assert "missing=1" in out4, out4
    states = dict(db.execute("SELECT path, state FROM photos").fetchall())
    assert states[os.path.join(root, "sub", "b.jpg")] == "missing", states
    assert states[a] == "queued", states

    # unwritable data dir -> nonzero exit, no fake success (CodeRabbit #41)
    bad = os.path.join(tmp, "bad-data")
    os.makedirs(bad)
    os.chmod(bad, 0o555)
    with open(os.path.join(tmp, "bad.conf"), "w") as f:
        f.write(f"roots={root}\ndata_dir={bad}\n")
    r = subprocess.run([bin_path, "scan", "--config", os.path.join(tmp, "bad.conf")],
                       capture_output=True, text=True)
    if os.geteuid() != 0:  # root ignores the mode bits
        assert r.returncode != 0, "persistence failure must exit nonzero"
    os.chmod(bad, 0o755)

    # depth cap (CodeRabbit #41 round 2): a previously indexed deep path
    # must NOT be swept missing by a capped scan.
    deep = os.path.join(root, "x1", "x2", "x3", "x4")
    os.makedirs(deep)
    with open(os.path.join(deep, "deep.jpg"), "w") as f:
        f.write("deep")
    out5 = run(bin_path, conf)  # default cap 8 covers depth 4
    assert "new=1" in out5, out5
    r = subprocess.run([bin_path, "scan", "--config", conf, "--max-depth", "2"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert "INCOMPLETE (depth cap reached)" in r.stdout, r.stdout
    assert "missing=0" in r.stdout, r.stdout
    states = dict(db.execute("SELECT path, state FROM photos").fetchall())
    assert states[os.path.join(deep, "deep.jpg")] == "queued", states

    # Empty roots (issue #108): the fresh-install default and a cleared
    # list are a successful no-op walk, never an error, and the existing
    # index is left byte-for-byte alone (no false missing sweep).
    empty_data = os.path.join(tmp, "empty-data")
    os.makedirs(empty_data)
    econf = os.path.join(tmp, "empty.conf")
    with open(econf, "w") as f:
        f.write(f"roots=\ndata_dir={empty_data}\n")
    r = subprocess.run([bin_path, "scan", "--config", econf],
                       capture_output=True, text=True)
    assert r.returncode == 0, f"empty-roots scan failed: {r.stderr}"
    assert "no roots configured" in r.stdout, r.stdout
    assert r.stderr == "", r.stderr
    assert not os.path.exists(os.path.join(empty_data, "index.db")), \
        "no-op scan must not create an index"

    cleared = os.path.join(tmp, "cleared.conf")
    with open(cleared, "w") as f:
        f.write(f"roots=\ndata_dir={data}\n")
    before = dict(db.execute("SELECT path, state FROM photos").fetchall())
    r = subprocess.run([bin_path, "scan", "--config", cleared],
                       capture_output=True, text=True)
    assert r.returncode == 0, f"cleared-roots scan failed: {r.stderr}"
    assert "no roots configured" in r.stdout, r.stdout
    after = dict(db.execute("SELECT path, state FROM photos").fetchall())
    assert after == before, (before, after)

    print("test_indexer: all assertions passed")

if __name__ == "__main__":
    main()
