# Incremental Scanning — Design (issue #37)

Status: draft v0.1 · 2026-10-28 · Owner: the owner

DESIGN.md mandates "incremental scanning by mtime delta, never a blind full
re-walk" (bounded-everything principle) and assigns the SQLite index +
incremental rescan to the Indexer milestone. This doc fixes the rules BEFORE
code: what is stored, when a photo is (re)processed, what invalidates stored
face data, and how deletions and renames behave.

## 1. Scope decision

- The **demo** (0.2.x, one folder, max 1000 photos) keeps its current
  full-scan behavior: it is a capability demo, not the index.
- Incremental scanning lands with the **Indexer milestone** (0.3.0): a
  persistent index over owner-selected folders, scanned in the background.
- This doc covers the index design only; the web UI surfaces (people, naming,
  search) read the index but are specified in DESIGN.md sections 3-5.

## 2. Index store

- SQLite (C amalgamation, statically linked), database in the app data dir on
  the RAID volume (per DESIGN.md architecture diagram).
- `PRAGMA journal_mode=WAL`, `PRAGMA secure_delete=ON` (privacy section of
  DESIGN.md), `PRAGMA synchronous=NORMAL`.
- Writes batched in transactions (per-photo results commit in chunks of ~100)
  so a crash loses at most a partial chunk, and fsync pressure stays low.
- Schema version row (`meta` table, semver); migrations run at app start
  (upgrade keeps data; DESIGN.md section on upgrades).

## 3. Schema sketch

```
meta(key TEXT PRIMARY KEY, value TEXT)           -- schema_version, model versions
photos(
  id INTEGER PRIMARY KEY,
  path TEXT NOT NULL UNIQUE,                     -- canonical /shares/... path
  size INTEGER NOT NULL,
  mtime INTEGER NOT NULL,                        -- seconds, from stat
  content_hash BLOB,                             -- BLAKE2s-128, lazily computed
  state TEXT NOT NULL,                           -- queued|scanned|no_faces|failed|missing
  scanned_at INTEGER,
  fail_reason TEXT,
  detector_ver TEXT NOT NULL,
  embedder_ver TEXT NOT NULL
)
faces(
  id INTEGER PRIMARY KEY,
  photo_id INTEGER NOT NULL REFERENCES photos(id) ON DELETE CASCADE,
  bbox TEXT NOT NULL,                            -- x,y,w,h
  embedding BLOB NOT NULL,                       -- float32 x N
  crop_path TEXT NOT NULL,                       -- face thumbnail, RAID data dir
  cluster_id INTEGER REFERENCES clusters(id)
)
clusters(id INTEGER PRIMARY KEY, name TEXT, merged_into INTEGER)
scan_jobs(id INTEGER PRIMARY KEY, started INTEGER, finished INTEGER,
          root TEXT, scanned INTEGER, new INTEGER, changed INTEGER, skipped INTEGER)
```

`scan_jobs` doubles as the run-history source for issue #36 in the product
stage (the demo's own light history stays a separate, simpler mechanism).

**Multi-user visibility:** every `photos` row's top-level share is derivable
from its canonical path (`/shares/<share>/...`). Per-user face-group
visibility is enforced at READ time against that share tag (issue #39):
a NAS user sees only clusters/photos from shares they can access - no
per-user index copies.

## 4. The rescan rule

A photo is (re)processed **only** when:

1. its canonical path is not in `photos` (new), or
2. `size` or `mtime` differ from the stored row (changed), or
3. its `state` is `failed` (retry once per run, then back off), or
4. `detector_ver`/`embedder_ver` differ from the running models (upgrade).

Otherwise the photo is **skipped** and its stored faces/embeddings are
reused. Directory walking is streaming (no whole-library list in RAM, per
the bounded-everything principle); the mtime/size stat per file is the only
per-unchanged-photo cost.

**mtime-ambiguous case:** some backup/restore flows rewrite files preserving
mtime. When size+mtime match but the owner suspects silent change, a manual
"re-verify folder" action computes `content_hash` (BLAKE2s-128) for that
folder and reprocesses mismatches. The hash is computed lazily only for this
action - never for every file on every run.

## 5. Invalidation on model upgrades

- `detector_ver` changes -> affected photos re-detect AND re-embed (boxes
  move), replacing their `faces` rows.
- `embedder_ver` changes only -> re-embed from the stored boxes (no
  re-decode of detection); clusters recomputed.
- Decode-library upgrades do NOT invalidate (pixel-equivalent output is
  assumed; the on-device self-test validates decode determinism per release).
- Reclustering after invalidation reuses embeddings still marked current.

## 6. Deletions and renames

- A `photos` row whose path no longer stats is marked `missing` (not
  deleted): the owner chooses in the UI whether to purge missing rows
  (faces cascade) or keep them for a temporarily-unmounted drive.
- Rename detection (cheap relink): a new path whose size+mtime+content_hash
  (computed on demand for candidates) matches a `missing` row relinks the
  row instead of reprocessing. Candidates are only rows currently marked
  missing, so the steady-state cost is zero.

## 7. Resource discipline (from DESIGN.md, restated for the indexer)

- nice 19 / ionice idle, worker thread cap, default <= 80% CPU/RAM budget,
  adaptive throttle on system load, schedule window default 01:00-07:00,
  pause/resume. The incremental design keeps idle-day rescans near-free
  (stat-only), so the schedule window mostly matters for new imports.

## 8. Interaction with the demo stage

- No shared state: the demo writes only to `demo-out/` and never touches the
  index DB. The demo's results page (issue #35) keeps reading `demo-out`.
- When the Indexer ships, the demo page gains a note pointing to the real
  index; the demo stays available as a quick on-device capability check.

## 9. Acceptance for the implementation milestone

- Second run over an unchanged folder processes 0 photos (stat-only walk).
- Touching one photo's mtime reprocesses exactly that photo.
- Deleting a photo marks it `missing`; purge removes its faces and crops.
- A rename relinks without reprocessing (hash-confirmed).
- Model version bump reprocesses exactly the invalidated stage.
- All of the above verified by on-device self-test cases, not only CI.
