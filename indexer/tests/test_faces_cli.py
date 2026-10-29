#!/usr/bin/env python3
"""CLI dispatch tests for wdf_faces (issue #65): each subcommand, bad
ids, missing args, and cross-subcommand flags, asserting exit codes and
output shape. Run: python3 indexer/tests/test_faces_cli.py /path/to/binary"""
import json
import os
import subprocess
import sys
import tempfile

BIN = sys.argv[1]
fails = 0


def check(cond, label):
    global fails
    if not cond:
        fails += 1
        print(f"FAIL: {label}", file=sys.stderr)


def run(*args):
    return subprocess.run([BIN, *args], capture_output=True, text=True)


tmp = tempfile.mkdtemp(prefix="wdf-cli-")
data = os.path.join(tmp, "data")
os.mkdir(data)
conf = os.path.join(tmp, "wfi.conf")
with open(conf, "w") as f:
    f.write(f"roots={tmp}\ndata_dir={data}\n")

# Dispatch and usage.
r = run()
check(r.returncode == 2 and "usage:" in r.stderr, "no args -> usage, exit 2")
r = run("bogus", "x")
check(r.returncode == 2 and "usage:" in r.stderr, "bad subcommand -> exit 2")
r = run("--version")
check(r.returncode == 0 and r.stdout.startswith("wdf_faces "), "--version ok")

# Unknown / cross-subcommand / malformed options are refused before work.
r = run("people", "--config", conf, "--bogus", "1")
check(r.returncode == 2 and "unknown option" in r.stderr, "unknown option refused")
r = run("people", "--config", conf, "--limit", "5")
check(r.returncode == 2 and "not valid for people" in r.stderr,
      "people refuses --limit")
r = run("cluster", "--config", conf, "--limit", "3")
check(r.returncode == 2 and "not valid for cluster" in r.stderr,
      "cluster refuses --limit")
r = run("process", "--config", conf, "--threshold", "0.5")
check(r.returncode == 2 and "not valid for process" in r.stderr,
      "process refuses --threshold")

# The process dispatch path itself (CodeRabbit #76): with valid options
# the run must reach process_queued - in this binary the stub, which
# fails with its marker error. A dispatch break cannot pass this suite.
r = run("process", "--config", conf, "--models", "/m")
check(r.returncode == 1 and "process failed: process_queued stub" in r.stderr,
      "valid process options reach the stub")
r = run("process", "--config", conf)
check(r.returncode == 2 and "no models dir" in r.stderr,
      "process without models refused")
r = run("person", "--config", conf, "--id", "abc")
check(r.returncode == 2 and "bad --id" in r.stderr, "junk id refused")
r = run("person", "--config", conf, "--id", "0")
check(r.returncode == 2 and "bad --id" in r.stderr, "id 0 refused")
r = run("person", "--config", conf, "--id", "99999999999999999999999")
check(r.returncode == 2 and "bad --id" in r.stderr, "overflow id refused")

# Missing required args per subcommand.
r = run("person", "--config", conf)
check(r.returncode == 2 and "person needs --id" in r.stderr, "person needs --id")
r = run("rename", "--config", conf)
check(r.returncode == 2 and "rename needs --id" in r.stderr, "rename needs --id")
r = run("merge", "--config", conf, "--from", "1")
check(r.returncode == 2 and "merge needs --from N --to N" in r.stderr,
      "merge missing --to refused")
r = run("merge", "--config", conf, "--to", "2")
check(r.returncode == 2 and "merge needs --from N --to N" in r.stderr,
      "merge missing --from refused")

# merge --from/--to values validated independently.
r = run("merge", "--config", conf, "--from", "abc", "--to", "2")
check(r.returncode == 2 and "bad --from" in r.stderr, "junk --from refused")
r = run("merge", "--config", conf, "--from", "1", "--to", "xyz")
check(r.returncode == 2 and "bad --to" in r.stderr, "junk --to refused")
r = run("merge", "--config", conf, "--from", "0", "--to", "2")
check(r.returncode == 2 and "bad --from" in r.stderr, "--from 0 refused")
r = run("merge", "--config", conf, "--from", "1", "--to", "-1")
check(r.returncode == 2 and "bad --to" in r.stderr, "negative --to refused")

# Read subcommands on an empty index: exit 0 with empty JSON shapes.
r = run("people", "--config", conf)
check(r.returncode == 0 and json.loads(r.stdout) == [], "people lists empty")
r = run("person", "--config", conf, "--id", "5")
got = json.loads(r.stdout) if r.returncode == 0 else None
check(r.returncode == 0 and got == {"id": 5, "name": "", "faces": []},
      "unknown person -> empty JSON person")

# Writes against an empty index fail with exit 1 AND the DB error text.
r = run("rename", "--config", conf, "--id", "5", "--name", "Dana")
check(r.returncode == 1 and "no such live person" in r.stderr,
      "rename of unknown cluster -> exit 1 + error text")
r = run("merge", "--config", conf, "--from", "1", "--to", "2")
check(r.returncode == 1 and "no such live person" in r.stderr,
      "merge of unknown clusters -> exit 1 + error text")

# Seeded index (clusters inserted directly): a successful rename persists
# and merge has real semantics (self-merge error, successful merge).
import sqlite3
dbp = os.path.join(data, "index.db")
db = sqlite3.connect(dbp)
cur = db.cursor()
cur.execute("INSERT INTO clusters(name) VALUES('Old')")
old_id = cur.lastrowid
cur.execute("INSERT INTO clusters(name) VALUES('Keep')")
keep_id = cur.lastrowid
# One face attached to old_id, so the merge must move it (CodeRabbit #76).
cur.execute(
    "INSERT INTO photos(root, path, size, mtime, state, detector_ver,"
    " embedder_ver) VALUES(?, ?, 8, 1000, 'scanned', 't', 't')",
    (tmp, os.path.join(tmp, "seed.jpg")))
photo_id = cur.lastrowid
cur.execute(
    "INSERT INTO faces(photo_id, bbox, embedding, crop_path, cluster_id)"
    " VALUES(?, '0,0,1,1', x'00000000', 'crops/seed.jpg', ?)",
    (photo_id, old_id))
db.commit()
db.close()

# rename without --name is refused before it can clear the live name
# (CodeRabbit #76).
r = run("rename", "--config", conf, "--id", str(old_id))
check(r.returncode == 2 and "needs --name" in r.stderr,
      "rename without --name refused")
r = run("people", "--config", conf)
people = json.loads(r.stdout) if r.returncode == 0 else []
still = [p for p in people if p["id"] == old_id]
check(still and still[0]["name"] == "Old", "refused rename keeps the name")

r = run("rename", "--config", conf, "--id", str(old_id), "--name", "Dana")
check(r.returncode == 0, "rename of a live cluster succeeds")
r = run("people", "--config", conf)
people = json.loads(r.stdout) if r.returncode == 0 else []
renamed = [p for p in people if p["id"] == old_id]
check(renamed and renamed[0]["name"] == "Dana", "rename persists in the index")

r = run("merge", "--config", conf, "--from", str(old_id), "--to", str(old_id))
check(r.returncode == 1 and "itself" in r.stderr, "self-merge refused")

r = run("merge", "--config", conf, "--from", str(old_id), "--to", str(keep_id))
check(r.returncode == 0, "merge of two live clusters succeeds")
r = run("people", "--config", conf)
people = json.loads(r.stdout) if r.returncode == 0 else []
check(all(p["id"] != old_id for p in people), "merged-away person is gone")
kept = [p for p in people if p["id"] == keep_id]
check(kept and kept[0]["name"] == "Keep", "named merge target keeps its name")
check(kept and kept[0]["faces"] == 1, "merge moved the face to the target")
r = run("person", "--config", conf, "--id", str(keep_id))
person = json.loads(r.stdout) if r.returncode == 0 else {}
check(person.get("faces") and person["faces"][0]["crop"] == "crops/seed.jpg",
      "moved face listed under the target person")

# cluster over the seeded index prints the counts line: the one face is
# a singleton group (its 4-byte embedding is below the dimension guard's
# comparable peers - there are none - so it stands alone).
r = run("cluster", "--config", conf)
check(r.returncode == 0 and r.stdout.strip() ==
      "faces=1 people=1 singletons=1", "cluster counts line")

# gc (issue #47): orphan crop files are removed, referenced ones stay,
# directories are untouched, and the counts line reports both.
crops = os.path.join(data, "crops")
os.mkdir(crops)
with open(os.path.join(crops, "seed.jpg"), "w") as f:
    f.write("x")  # referenced by the merged face above
with open(os.path.join(crops, "orphan.jpg"), "w") as f:
    f.write("x")
os.mkdir(os.path.join(crops, "keepdir"))
r = run("gc", "--config", conf, "--limit", "3")
check(r.returncode == 2 and "not valid for gc" in r.stderr,
      "gc refuses --limit")
r = run("gc", "--config", conf)
check(r.returncode == 0 and r.stdout.strip() == "removed=1 kept=1 failed=0",
      "gc counts line")
check(not os.path.exists(os.path.join(crops, "orphan.jpg")),
      "orphan crop removed")
check(os.path.exists(os.path.join(crops, "seed.jpg")),
      "referenced crop kept")
check(os.path.isdir(os.path.join(crops, "keepdir")),
      "directory untouched by gc")
import shutil
shutil.rmtree(crops)
r = run("gc", "--config", conf)
check(r.returncode == 0 and r.stdout.strip() == "removed=0 kept=0 failed=0",
      "absent crops dir reports the full counts shape")
os.mkdir(crops)
# A symlink at crops/ is refused: the sweep must never follow it into
# another writable directory (CodeRabbit #99).
elsewhere = os.path.join(tmp, "elsewhere")
os.mkdir(elsewhere)
with open(os.path.join(elsewhere, "keepme.jpg"), "w") as f:
    f.write("x")
shutil.rmtree(crops)
os.symlink(elsewhere, crops)
r = run("gc", "--config", conf)
check(r.returncode == 1 and "not a real directory" in r.stderr,
      "symlinked crops dir refused")
check(os.path.exists(os.path.join(elsewhere, "keepme.jpg")),
      "symlink target contents untouched")
os.unlink(crops)
os.mkdir(crops)

# Instance lock (issues #46, #98): a held flock makes mutating commands
# exit politely while the read commands stay usable; release frees it.
import fcntl
lock_path = os.path.join(data, "wdf.lock")
lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT)
fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
r = run("process", "--config", conf)
check(r.returncode == 1 and "already running" in r.stderr,
      "second process run refused politely")
r = run("rename", "--config", conf, "--id", str(keep_id), "--name", "X")
check(r.returncode == 1 and "already running" in r.stderr,
      "rename under a live lock refused")
r = run("gc", "--config", conf)
check(r.returncode == 1 and "already running" in r.stderr,
      "gc under a live lock refused")
r = run("people", "--config", conf)
check("already running" not in r.stderr, "read command stays lock-free")
os.close(lock_fd)
r = run("cluster", "--config", conf)
check(r.returncode == 0 and "already running" not in r.stderr,
      "released lock lets a mutating run through")

# gc failure accounting (review #99): an unremovable crop is counted,
# named on stderr, and fails the run.
with open(os.path.join(crops, "stuck.jpg"), "w") as f:
    f.write("x")
os.chmod(crops, 0o555)
try:
    r = run("gc", "--config", conf)
finally:
    os.chmod(crops, 0o755)
if os.path.exists(os.path.join(crops, "stuck.jpg")):
    # The mode really blocked the unlink: the failure path ran.
    check(r.returncode == 1 and "failed=1" in r.stdout and
          "stuck.jpg" in r.stderr, "failed unlink counted and named")
    os.remove(os.path.join(crops, "stuck.jpg"))
else:
    # Permission override (root, CAP_DAC_OVERRIDE, ...) - 0555 could not
    # block the unlink (review #101), so there is no failure to assert;
    # the probe result replaces an euid guess.
    check(r.returncode == 0 and "failed=0" in r.stdout,
          "unremovable crop unlinked under privilege override")

# Empty roots (issue #108): only the walk no-ops - every wdf_faces DB
# command keeps operating on the existing index.
econf = os.path.join(tmp, "empty.conf")
with open(econf, "w") as f:
    f.write(f"roots=\ndata_dir={data}\n")
r = run("people", "--config", econf)
people = json.loads(r.stdout) if r.returncode == 0 else []
check(r.returncode == 0 and any(p["id"] == keep_id for p in people),
      "people reads the index with empty roots")
r = run("person", "--config", econf, "--id", str(keep_id))
person = json.loads(r.stdout) if r.returncode == 0 else {}
check(r.returncode == 0 and person.get("faces"), "person reads with empty roots")
r = run("rename", "--config", econf, "--id", str(keep_id), "--name", "EmptyOK")
check(r.returncode == 0, "rename writes with empty roots")
r = run("people", "--config", econf)
people = json.loads(r.stdout) if r.returncode == 0 else []
kept = [p for p in people if p["id"] == keep_id]
check(kept and kept[0]["name"] == "EmptyOK", "empty-roots rename persisted")
r = run("cluster", "--config", econf)
check(r.returncode == 0 and r.stdout.startswith("faces="),
      "cluster runs with empty roots")
r = run("gc", "--config", econf)
check(r.returncode == 0 and r.stdout.startswith("removed="),
      "gc runs with empty roots")
r = run("process", "--config", econf, "--models", "/m")
check(r.returncode == 1 and "process failed: process_queued stub" in r.stderr,
      "process still dispatches with empty roots")

# merge under empty roots (CodeRabbit #109): the other write with real
# semantics - the source cluster must be gone, the target keeps the faces.
db = sqlite3.connect(dbp)
cur = db.cursor()
cur.execute("INSERT INTO clusters(name) VALUES('Second')")
second_id = cur.lastrowid
cur.execute(
    "INSERT INTO faces(photo_id, bbox, embedding, crop_path, cluster_id)"
    " VALUES(?, '1,1,2,2', x'00000000', 'crops/seed2.jpg', ?)",
    (photo_id, second_id))
db.commit()
db.close()
r = run("merge", "--config", econf, "--from", str(second_id),
        "--to", str(keep_id))
check(r.returncode == 0, "merge succeeds with empty roots")
r = run("people", "--config", econf)
people = json.loads(r.stdout) if r.returncode == 0 else []
check(all(p["id"] != second_id for p in people),
      "merged-away source gone with empty roots")
kept = [p for p in people if p["id"] == keep_id]
check(kept and kept[0]["faces"] == 2,
      "empty-roots merge moved the face to the target")
r = run("person", "--config", econf, "--id", str(keep_id))
person = json.loads(r.stdout) if r.returncode == 0 else {}
crops = {f.get("crop") for f in person.get("faces", [])}
check(r.returncode == 0 and crops == {"crops/seed.jpg", "crops/seed2.jpg"},
      "the actual moved crop is listed under the target")

# Missing photos are out of everything identity-shaped (root removal,
# #102): people counts, person details and cluster input exclude their
# faces - but the rows AND the cluster membership stay, so a restored
# photo rejoins the SAME person (name preserved).
db = sqlite3.connect(dbp)
db.execute("UPDATE photos SET state='missing' WHERE id=?", (photo_id,))
# (The full missing -> restore lifecycle through scan -> replace_faces ->
# cluster is pinned in indexer_test.cpp's "Restore keeps identity" block;
# here the direct UPDATE isolates the QUERY exclusion semantics.)
db.commit()
db.close()
r = run("people", "--config", conf)
people = json.loads(r.stdout) if r.returncode == 0 else []
kept = [p for p in people if p["id"] == keep_id]
check(kept and kept[0]["faces"] == 0,
      "people count excludes missing-photo faces")
r = run("person", "--config", conf, "--id", str(keep_id))
person = json.loads(r.stdout) if r.returncode == 0 else {}
check(r.returncode == 0 and person.get("faces") == [],
      "person details exclude missing-photo faces")
r = run("cluster", "--config", conf)
check(r.returncode == 0 and r.stdout.strip() ==
      "faces=0 people=0 singletons=0", "cluster input excludes missing faces")
db = sqlite3.connect(dbp)
row = db.execute("SELECT COUNT(*) FROM faces WHERE cluster_id IS NULL"
                 ).fetchone()
name = db.execute("SELECT name FROM clusters WHERE id=?",
                  (keep_id,)).fetchone()
db.close()
check(row[0] == 0, "missing faces keep their cluster membership")
check(name is not None and name[0] == "EmptyOK",
      "all-missing person survives a rebuild with its name")
r = run("people", "--config", conf)
people = json.loads(r.stdout) if r.returncode == 0 else []
kept = [p for p in people if p["id"] == keep_id]
check(kept and kept[0]["faces"] == 0 and kept[0]["name"] == "EmptyOK",
      "all-missing person still listed at zero faces")
db = sqlite3.connect(dbp)
db.execute("UPDATE photos SET state='scanned' WHERE id=?", (photo_id,))
db.commit()
db.close()
r = run("people", "--config", conf)
people = json.loads(r.stdout) if r.returncode == 0 else []
kept = [p for p in people if p["id"] == keep_id]
check(kept and kept[0]["faces"] == 2,
      "restored photo rejoins the same person")

if fails:
    print(f"test_faces_cli: {fails} FAILURES", file=sys.stderr)
    sys.exit(1)
print("test_faces_cli: all assertions passed")
