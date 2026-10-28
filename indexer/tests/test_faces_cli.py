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
    " embedder_ver) VALUES(?, ?, 8, 1000, 'done', 't', 't')",
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

if fails:
    print(f"test_faces_cli: {fails} FAILURES", file=sys.stderr)
    sys.exit(1)
print("test_faces_cli: all assertions passed")
