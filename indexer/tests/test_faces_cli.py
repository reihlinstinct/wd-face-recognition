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
      "merge needs both ends")

# Read subcommands on an empty index: exit 0 with empty JSON shapes.
r = run("people", "--config", conf)
check(r.returncode == 0 and json.loads(r.stdout) == [], "people lists empty")
r = run("person", "--config", conf, "--id", "5")
got = json.loads(r.stdout) if r.returncode == 0 else None
check(r.returncode == 0 and got == {"id": 5, "name": "", "faces": []},
      "unknown person -> empty JSON person")

# Writes against an empty index fail with exit 1 (DB error), not a crash.
r = run("rename", "--config", conf, "--id", "5", "--name", "Dana")
check(r.returncode == 1, "rename of unknown cluster -> exit 1")
r = run("merge", "--config", conf, "--from", "1", "--to", "2")
check(r.returncode == 1, "merge of unknown clusters -> exit 1")

# cluster on the empty index prints the counts line.
r = run("cluster", "--config", conf)
check(r.returncode == 0 and r.stdout.strip() ==
      "faces=0 people=0 singletons=0", "cluster counts line")

if fails:
    print(f"test_faces_cli: {fails} FAILURES", file=sys.stderr)
    sys.exit(1)
print("test_faces_cli: all assertions passed")
