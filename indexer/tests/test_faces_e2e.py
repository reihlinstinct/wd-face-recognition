#!/usr/bin/env python3
"""End-to-end test for wdf_indexer + wdf_faces (issue #40 PR 2): real scan,
real pipeline (SCRFD + MobileFaceNet), real SQLite index. Covers queue ->
process -> reprocess (mtime change) -> missing, crop files on disk, and the
numerical sanity of the detect -> align -> embed path (identical photos
must embed to ~1.0 cosine, different people well below).

Run: python3 indexer/tests/test_faces_e2e.py /path/to/wdf_indexer /path/to/wdf_faces <photos_dir> <models_dir>
photos_dir must contain the pinned portrait fixtures (lincoln/hopper/einstein).
"""
import os
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile

def run(args, expect=0):
    r = subprocess.run(args, capture_output=True, text=True)
    assert r.returncode == expect, f"{args} -> {r.returncode}: {r.stderr}"
    return r.stdout

def emb_cosine(a, b):
    dot = sum(x * y for x, y in zip(a, b))
    na = sum(x * x for x in a) ** 0.5
    nb = sum(x * x for x in b) ** 0.5
    return dot / (na * nb)

def main():
    indexer, faces, photos_src, models = sys.argv[1:5]
    tmp = tempfile.mkdtemp(prefix="wff-e2e-")
    root = os.path.join(tmp, "photos")
    data = os.path.join(tmp, "data")
    shutil.copytree(photos_src, root)
    os.makedirs(data)  # the indexer expects data_dir to exist (a RAID volume dir)
    # A byte-identical copy of einstein: the same face twice.
    shutil.copy(os.path.join(root, "einstein.jpg"),
                os.path.join(root, "einstein_copy.jpg"))
    with open(os.path.join(root, "garbage.jpg"), "wb") as f:
        f.write(os.urandom(2000))
    conf = os.path.join(tmp, "wfi.conf")
    with open(conf, "w") as f:
        f.write(f"roots={root}\ndata_dir={data}\nmodels_dir={models}\n")

    out = run([indexer, "scan", "--config", conf])
    assert "new=5" in out, out  # 3 portraits + einstein copy + garbage
    out = run([faces, "process", "--config", conf])
    print(out)
    assert "processed=4" in out and "failed=1" in out, out
    assert "remaining=0" in out, out

    db = sqlite3.connect(os.path.join(data, "index.db"))
    states = dict(db.execute("SELECT path, state FROM photos").fetchall())
    byname = {os.path.basename(k): v for k, v in states.items()}
    assert byname["garbage.jpg"] == "failed", byname
    assert all(v == "scanned" for k, v in byname.items() if k != "garbage.jpg"), byname

    face_rows = db.execute(
        "SELECT p.path, f.bbox, f.embedding, f.crop_path FROM faces f"
        " JOIN photos p ON p.id = f.photo_id").fetchall()
    assert len(face_rows) >= 4, face_rows  # one face per portrait + copy
    for path, bbox, emb, crop in face_rows:
        assert len(emb) % 4 == 0 and len(emb) >= 128 * 4, (path, len(emb))
        assert len(bbox.split(",")) == 4, bbox
        assert not crop.startswith("/") and ".." not in crop, crop
        assert os.path.isfile(os.path.join(data, crop)), crop

    embs = {}
    for path, bbox, emb, crop in face_rows:
        embs.setdefault(os.path.basename(path), []).append(
            struct.unpack(f"<{len(emb)//4}f", emb))
    same = emb_cosine(embs["einstein.jpg"][0], embs["einstein_copy.jpg"][0])
    diff = emb_cosine(embs["einstein.jpg"][0], embs["lincoln.jpg"][0])
    print(f"cosine same={same:.4f} different={diff:.4f}")
    assert same > 0.99, f"identical photo must embed identically: {same}"
    assert diff < 0.5, f"different people must not cluster: {diff}"

    # mtime change on one photo -> re-queued, re-processed, crops replaced.
    einstein = os.path.join(root, "einstein.jpg")
    os.utime(einstein, None)
    out = run([indexer, "scan", "--config", conf])
    assert "changed=1" in out, out
    out = run([faces, "process", "--config", conf])
    assert "processed=1" in out, out
    n_faces = db.execute("SELECT count(*) FROM faces").fetchone()[0]
    assert n_faces == len(face_rows), (n_faces, len(face_rows))

    # Deletion -> missing; rows stay (read-time filtering is PR 5).
    os.remove(os.path.join(root, "hopper.jpg"))
    out = run([indexer, "scan", "--config", conf])
    assert "missing=1" in out, out
    state = db.execute("SELECT state FROM photos WHERE path LIKE '%hopper.jpg'").fetchone()
    assert state[0] == "missing", state

    # data_dir/crops as a regular file: process must refuse up front
    # (CodeRabbit #45), not fail every photo's crop write one by one.
    bad = os.path.join(tmp, "bad-data")
    os.makedirs(bad)
    with open(os.path.join(bad, "crops"), "w") as f:
        f.write("not a dir")
    conf_bad = os.path.join(tmp, "wfi-bad.conf")
    with open(conf_bad, "w") as f:
        f.write(f"roots={root}\ndata_dir={bad}\nmodels_dir={models}\n")
    r = subprocess.run([faces, "process", "--config", conf_bad],
                       capture_output=True, text=True)
    assert r.returncode != 0 and "not a directory" in r.stderr, (r.returncode, r.stderr)
    print("test_faces_e2e: all assertions passed")

if __name__ == "__main__":
    main()
