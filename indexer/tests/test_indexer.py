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

    # One job per run, shared across roots (root removal, #102): a
    # two-root scan writes ONE scan_jobs row, and neither root's sweep
    # can mark the other root's photos (observed earlier in the SAME
    # run) missing.
    multi = os.path.join(tmp, "multi")
    ra, rb, mdata = (os.path.join(multi, d) for d in ("A", "B", "data"))
    os.makedirs(ra)
    os.makedirs(rb)
    os.makedirs(mdata)
    for d, name in ((ra, "pa.jpg"), (rb, "pb.jpg")):
        with open(os.path.join(d, name), "w") as f:
            f.write("x")
    mconf = os.path.join(multi, "wfi.conf")
    with open(mconf, "w") as f:
        f.write(f"roots={ra},{rb}\ndata_dir={mdata}\n")
    run(bin_path, mconf)
    mdb = sqlite3.connect(os.path.join(mdata, "index.db"))
    jobs = mdb.execute("SELECT root FROM scan_jobs").fetchall()
    assert jobs == [(f"{ra},{rb}",)], jobs
    os.unlink(os.path.join(ra, "pa.jpg"))
    out = run(bin_path, mconf)
    jobs = mdb.execute("SELECT root, missing FROM scan_jobs").fetchall()
    assert len(jobs) == 2 and jobs[1] == (f"{ra},{rb}", 1), jobs
    states = dict(mdb.execute("SELECT path, state FROM photos").fetchall())
    assert states[os.path.join(ra, "pa.jpg")] == "missing", states
    assert states[os.path.join(rb, "pb.jpg")] == "queued", states
    mdb.close()

    # Narrowing (docs/ROOTS-SETTINGS.md regression a): indexed under
    # Photos, narrowed to Photos/2020 - the still-covered photo keeps
    # its stored root and state via the shared job stamp; the rest flip
    # to missing under the unconfigured-root sweep.
    photos_dir = os.path.join(tmp, "narrow", "Photos")
    y20 = os.path.join(photos_dir, "2020")
    y21 = os.path.join(photos_dir, "2021")
    ndata = os.path.join(tmp, "narrow", "data")
    os.makedirs(y20)
    os.makedirs(y21)
    os.makedirs(ndata)
    for d in (y20, y21):
        with open(os.path.join(d, "p.jpg"), "w") as f:
            f.write("x")
    nconf = os.path.join(tmp, "narrow", "wfi.conf")
    with open(nconf, "w") as f:
        f.write(f"roots={photos_dir}\ndata_dir={ndata}\n")
    run(bin_path, nconf)
    with open(nconf, "w") as f:
        f.write(f"roots={y20}\ndata_dir={ndata}\n")
    out = run(bin_path, nconf)
    assert f"root={photos_dir} (unconfigured) missing=1" in out, out
    ndb = sqlite3.connect(os.path.join(ndata, "index.db"))
    states = dict(ndb.execute("SELECT path, state FROM photos").fetchall())
    assert states[os.path.join(y20, "p.jpg")] == "queued", states
    assert states[os.path.join(y21, "p.jpg")] == "missing", states
    roots_seen = dict(ndb.execute("SELECT path, root FROM photos").fetchall())
    assert roots_seen[os.path.join(y20, "p.jpg")] == photos_dir, roots_seen
    jobs = ndb.execute("SELECT root FROM scan_jobs ORDER BY id").fetchall()
    assert jobs == [(photos_dir,), (y20,)], jobs

    # Narrowing with an unreadable covered subdir (regression b): the
    # incomplete walk suppresses the unconfigured sweep too - nothing
    # may flip while coverage is partial.
    priv = os.path.join(y20, "priv")
    os.makedirs(priv)
    with open(os.path.join(priv, "q.jpg"), "w") as f:
        f.write("x")
    with open(nconf, "w") as f:
        f.write(f"roots={photos_dir}\ndata_dir={ndata}\n")
    run(bin_path, nconf)  # full coverage again: y21 + priv re-queued
    with open(nconf, "w") as f:
        f.write(f"roots={y20}\ndata_dir={ndata}\n")
    os.chmod(priv, 0)
    try:
        os.listdir(priv)
        blocked = False
    except PermissionError:
        blocked = True
    try:
        out = run(bin_path, nconf)
    finally:
        os.chmod(priv, 0o755)
    if blocked:
        # The mode really blocked opendir: both sweeps must stay off.
        assert "INCOMPLETE (unreadable dir)" in out, out
        assert "(unconfigured) sweep suppressed" in out, out
        states = dict(ndb.execute("SELECT path, state FROM photos").fetchall())
        assert all(st == "queued" for st in states.values()), states
    else:
        # Privilege override: the walk completed, normal narrowing rules.
        assert "INCOMPLETE" not in out, out
    ndb.close()

    # Empty roots (#108): the fresh-install default is a successful run,
    # never an error. The walk no-ops; the run still opens one job and
    # sweeps (nothing is stored yet), so the index appears with zero
    # photos and one finished job.
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
    edb = sqlite3.connect(os.path.join(empty_data, "index.db"))
    assert edb.execute("SELECT COUNT(*) FROM photos").fetchone()[0] == 0
    assert edb.execute("SELECT COUNT(*) FROM scan_jobs").fetchone()[0] == 1
    edb.close()

    # Cleared after indexing (root removal, #102): the zero-root run
    # sweeps EVERY stored root - all photos flip to missing under one
    # new shared job. Reversible: re-adding the root re-queues them
    # through the normal missing -> changed path.
    cleared = os.path.join(tmp, "cleared.conf")
    with open(cleared, "w") as f:
        f.write(f"roots=\ndata_dir={data}\n")
    before = dict(db.execute("SELECT path, state FROM photos").fetchall())
    jobs_before = db.execute("SELECT COUNT(*) FROM scan_jobs").fetchone()[0]
    r = subprocess.run([bin_path, "scan", "--config", cleared],
                       capture_output=True, text=True)
    assert r.returncode == 0, f"cleared-roots scan failed: {r.stderr}"
    assert "no roots configured" in r.stdout, r.stdout
    after = dict(db.execute("SELECT path, state FROM photos").fetchall())
    assert set(after) == set(before), (before, after)  # no rows lost
    assert all(st == "missing" for st in after.values()), after
    jobs_after = db.execute("SELECT COUNT(*) FROM scan_jobs").fetchone()[0]
    assert jobs_after == jobs_before + 1, (jobs_before, jobs_after)
    out = run(bin_path, conf)  # the original single-root config, restored
    # Unchanged files return WITHOUT re-queueing (reviewer #112): these
    # were never processed (scanned_at NULL) so they land back in
    # 'queued'; processed photos would return straight to 'scanned'.
    assert "restored=2" in out and "changed=0" in out, out
    states = dict(db.execute("SELECT path, state FROM photos").fetchall())
    # b.jpg is gone from disk and stays missing; the rest re-queue.
    assert states[os.path.join(root, "sub", "b.jpg")] == "missing", states
    assert states[a] == "queued", states
    assert states[os.path.join(deep, "deep.jpg")] == "queued", states

    print("test_indexer: all assertions passed")

if __name__ == "__main__":
    main()
