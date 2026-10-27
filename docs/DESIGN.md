# WD Face Recognition — Design Doc
**On-device face recognition for WD My Cloud PR4100, packaged as a native My Cloud OS 5 app**

Status: draft v0.26 · 2026-10-27 · Owner: the owner

---

## 1. Goal

A WD My Cloud OS 5 app for the PR4100 that scans **all photos on the device**,
detects faces, clusters similar faces into people, and serves a web page on the NAS
where the owner names each cluster — then can optionally write the person's name into
the photo's XMP metadata (section 4.3). **100% on-device**: no cloud calls, no
telemetry, no photos or embeddings ever leave the box. Installable the way any WD app
is installed: Settings → Apps → Install an app manually.

## 2. Target hardware and OS (grounded - see Sources)

| Fact | Value | Source |
|---|---|---|
| CPU | Intel Pentium N3710, 4 cores / 4 threads, 1.6 GHz (burst 2.56 GHz), Braswell (Airmont), 14 nm, 6 W TDP | Intel ARK; WD product spec sheet |
| ISA ceiling | SSE4.2 — **no AVX/AVX2** (Airmont core) | Intel ARK (Instruction Set Extensions) |
| RAM | 4 GB DDR3L (2×2 GB), upgradeable to 16 GB | WD support / spec sheet |
| OS | My Cloud OS 5 — stripped Debian 11 (Bullseye), x86_64, no package manager, outdated system libraries | jessedp/WD-NAS-App-Builder |
| App format | `.bin` apkg bundle: `apkg.rc` + lifecycle scripts (`install.sh`, `init.sh`, `start.sh`, `stop.sh`, `remove.sh`), installed via the web dashboard, no SSH needed | WD NAS App Builder template |
| Build env | Debian 11 (Bullseye) Docker container to match the NAS; binaries must be statically linked or bundled | WD NAS App Builder |
| Official SDK | WD's developer portal/SDK appears discontinued; community packaging conventions are the de-facto standard | community.wd.com threads |

**Caveat:** OS5 internals and app-format details rest on community sources
(WD's official developer docs are offline); they are validated on the real
device in milestones 1-3 before we rely on them.

**Key consequence:** no AVX and 4 GB RAM rule out the usual Python + ONNX Runtime +
InsightFace stack (heavy RAM, prebuilt wheels tuned for AVX). The inference engine must
work well on SSE4.2-only silicon.

## 3. Feasibility verdict

**Feasible — as a background batch indexer, not a realtime service.**

- Detection: SCRFD-0.5GF / 2.5GF (InsightFace's lightweight detector family).
- Recognition: MobileFaceNet-class ArcFace model (≤ 4 MB, ~1M params, 99.5% LFW),
  e.g. the `w600k_mbf`/`buffalo_sc` recognition model.
- Runtime: **ncnn** (Tencent) — the best fit for SSE4.2-only low-end x86; small,
  dependency-free, statically linkable. OpenCV DNN is the fallback.
- Expected throughput on the N3710 (estimate, to be validated in milestone 2):
  ~0.5–2 s per photo for detection + ~0.2–0.5 s per face for embedding on 2–3 threads.
  A 20k–50k photo library indexes in one to a few nights. Incremental rescans are cheap.
- RAM budget for the worker: ≤ 1.5 GB, leaving headroom for OS 5 on the stock 4 GB.
  (Optional user RAM upgrade to 16 GB is a nice-to-have, never a requirement.)

## 4. Architecture

```
+---------------------- WD My Cloud PR4100 (OS 5) ----------------------+
|  apkg package "facerecog"                                             |
|                                                                       |
|  indexer daemon (C++, ncnn, statically linked)                        |
|    - discovers images across ALL shares (photos are spread           |
|      everywhere); owner-selected include/exclude filter on top,       |
|      default: NOTHING scanned until chosen in the UI                  |
|    - decode (libjpeg-turbo) -> SCRFD detect -> align -> MobileFaceNet |
|    - embeddings + face crops + photo metadata (EXIF date, GPS ->      |
|      OFFLINE reverse-geocoded place via bundled gazetteer) -> SQLite   |
|    - resource-capped: default <= 80% CPU/RAM budget (configurable),   |
|      nice 19 / ionice idle, thread limits, cgroup caps where the      |
|      kernel allows; adaptive throttle on system load (SMB/DLNA        |
|      activity, loadavg); pause/resume; schedule window (01:00-07:00)  |
|                                                                       |
|  clustering service                                                   |
|    - agglomerative clustering on embeddings, merge-on-name            |
|    - "people" table; unknown clusters surface as "Person 7"           |
|                                                                       |
|  local web UI (mobile-first, no external CDN/fonts)                   |
|    - served on a high port, e.g. :8088, LAN only                      |
|    - browse people, name/merge people, "all photos of X", per-photo   |
|      face view; Hebrew + English UI                                   |
|                                                                       |
|  data dir on the RAID volume: SQLite DB, face thumbnails, models      |
+-----------------------------------------------------------------------+
```

### Web UI access control
The UI can view private photos and mutate data (purge, sidecar writes) - "LAN only"
is not enough:

- **Authentication required for everything** (viewing included). v1: app-local
  login (owner-set password, argon2-hashed), session cookie (HttpOnly,
  SameSite=Strict, Secure when served over HTTPS). No anonymous mode.
- **First-run claim:** on first launch the UI demands password creation before
  anything else works - until then every route shows only the setup screen, so
  nobody else on the LAN can claim the app first.
- **Transport:** default is plain HTTP on :8088 - documented risk: the password
  crosses the LAN in cleartext. The app therefore offers a self-signed HTTPS
  option (cert generated on-device at setup, owner accepts the browser warning
  once); recommended when the LAN isn't fully trusted.
- **Bind address:** localhost + the NAS LAN interface only; never exposed to the
  internet by the app itself. No UPnP/NAT traversal.
- **CSRF:** token on every mutating endpoint (same-site cookies are not treated
  as sufficient alone).
- Rate-limited login; all mutating actions logged (activity log).

### Folder selection (include/exclude)
- **Default: nothing is scanned.** The indexer stays idle until the owner picks
  folders in the web UI.
- The UI lists the NAS shares/folder tree with checkboxes: include a folder
  (subfolders inherit) or exclude a folder inside an included tree.
- The list is stored in the app config; the indexer re-walks on change.
  Removing a folder stops future scans; its already-indexed faces stay until the
  owner chooses "purge data for this folder" in the UI.
- Excludes always win over includes.

### Resource cap (owner requirement)
Scanning must never starve the NAS: file serving and other apps keep priority.

- Configurable budget, default **<= 80% of CPU and RAM** (owner can lower/raise).
- Enforcement: worker thread/process limits, nice 19 / ionice idle, cgroup
  cpu/memory caps where the NAS kernel permits.
- **Adaptive throttling:** the indexer watches system load (loadavg, active
  SMB/DLNA sessions) and backs off or pauses while the device is busy serving
  files, resuming when idle. Manual pause/resume in the UI; configurable
  schedule window (default 01:00-07:00).
- **Verified, not assumed:** the on-device self-test runs a capped benchmark on
  the real PR4100 and reports actual CPU%/RSS against the cap.

### Image discovery across all shares
Photos are spread across every folder on the device, so discovery does not assume a
photo share:

- **Primary path (ours):** a full walk of the data volumes, filtered by image
  extensions + magic-byte check (JPEG/PNG/HEIC*/TIFF; *HEIC decode depends on
  libheif being bundleable - validated in the benchmark spike), then the
  include/exclude filter is applied. Robust, no dependencies on WD internals.
- **Optional accelerator (read-only reuse):** OS5's media indexing is Twonky DLNA
  (a separate OS5 app that indexes every share with Media Serving enabled), and the
  WD web/photo access keeps its own photo/thumbnail index DB. If present and
  readable, these can seed the candidate photo list instead of a cold full walk.
  **Read-only, best-effort only**: formats are undocumented and version-dependent,
  so the app treats them as a hint and falls back to its own walk. Validated on the
  real device in milestone 3 before any reliance; a failure here never blocks
  scanning. Sources: WD KB a_id/29819, a_id/30285; community.wd.com/t/photo-index-db/298167.

### Packaging
- Built in a Debian 11 Bullseye Docker container in CI, following the
  jessedp/WD-NAS-App-Builder template (actively maintained, tested on PR4100).
- Everything statically linked; zero reliance on NAS system libraries.
- **Install path (primary, by user decision): dashboard only.** The owner installs
  the `.bin` via the OS 5 web dashboard (Settings → Apps → Install an app manually).
  SSH is NOT part of the normal flow - development, CI and on-device testing all work
  without it (the owner installs test builds himself). If SSH access is ever needed,
  it will be requested explicitly with the exact reason, and credentials only via a
  vault link, never in chat.
- Uninstall removes binaries; the face DB is kept (with a "remove data too" option).

### Metadata write-back (XMP) — opt-in
Once a cluster is named, the app can record the name in standard photo metadata so it
survives outside the app (Lightroom/digiKam/Photos-compatible):

- Standard: XMP — IPTC Extension `PersonInImage` + MWG face regions
  (`mwg-rs:Regions` with `PersonDisplayName`), plus keywords. Library: **Exiv2**
  (C++, statically linkable, native XMP support).
- **v1 mode (chosen by the owner): sidecar `.xmp` only.** Originals are never
  touched; the sidecar sits in the SAME folder as the photo and shares its name.
  Works for RAW files (CR2/NEF/DNG) too.
- **Naming convention (verified):** two forms exist in the wild -
  `photo.xmp` (base name, extension stripped: Lightroom, digiKam, ExifTool default)
  and `photo.jpg.xmp` (full filename + .xmp: darktable; Immich's preferred form,
  with `photo.xmp` as its documented fallback - if both exist Immich reads
  `.jpg.xmp`). Sources: docs.immich.app XMP Sidecars; immich-app/immich PR #7813.
  -> v1 writes **`photo.xmp`** (widest tool support: Lightroom, digiKam, ExifTool,
     Immich fallback), with an app setting to write `photo.jpg.xmp` instead/additionally
     for darktable users. If a sidecar already exists, the app MERGES (adds its fields,
     preserves everything else) instead of overwriting.
- **Edge-case rules:**
  - RAW+JPEG pairs (`IMG_1234.CR2` + `IMG_1234.JPG`) collide on `IMG_1234.xmp`:
    when both exist, the app writes the full-filename form for both
    (`IMG_1234.CR2.xmp`, `IMG_1234.JPG.xmp`); single-file basenames use `photo.xmp`.
  - An existing sidecar that fails to parse is NEVER overwritten or "fixed": the
    file is skipped, logged, and surfaced in the UI for the owner to decide.
  - Writes are atomic: temp file + fsync + rename; a crash mid-write cannot leave
    a truncated sidecar.
  - The FIRST write batch runs as a dry-run: the UI shows exactly what would be
    written and requires confirmation before any real write.
  - Removal strips ONLY the app's own fields (tracked in the DB): the
    PersonInImage entries and MWG regions the app wrote; pre-existing fields and
    other tools' data stay.
- Names are always also stored in the app's own DB; the sidecar is the interop copy,
  not the source of truth.
- **Google Photos interop (verified):** person names do NOT carry into Google
  Photos - not via sidecars (ignored on upload) and not via embedded XMP (Google
  Photos reads Caption/Description and date/location, but not IPTC PersonInImage
  or MWG face regions; its API has no people read/write and its policy forbids
  third-party face clustering). Google does its own face grouping. Optional
  workaround in the app: ALSO write names into the standard Description/keywords
  text fields on export copies, so they appear as text in Google Photos' info
  panel - never on the NAS originals unless the owner asks.
  Sources: support.google.com/photos/thread/93983534; developers.google.com/photos
  API policy; StackOverflow q/63128115.
- **Visibility (verified):** sidecars are visible regular files - they appear in the
  WD Files view and in SMB/network browsing, but NOT in the WD "Photos" tab (that
  lists indexed media only). Dot-prefix hiding would break interop (other tools won't
  find them) and there is no dashboard setting to hide files by extension; Samba
  veto/hide rules would need smb.conf edits over SSH and could break sidecar access
  over SMB - rejected.
- **Deferred:** embedded XMP (writing into the JPEG/TIFF itself, with backup) is
  kept as a possible later opt-in **only if the owner asks** - it modifies original
  files, so it stays out of v1.
- Writing is never automatic: the owner enables it per person or globally in the UI,
  and every write batch is logged (file list + sidecar hashes) in the app log.

### Naming: UI flow and name format
**UI flow (mobile-first, Hebrew RTL + English):**
- **Search-by-name box at the top:** matches display name and sort/search name,
  Hebrew and latin, prefix + substring; selecting a person opens their photos.
  Unnamed clusters are reachable from the People page below.
- **Multi-person search:** names become chips in the search box. Multiple chips
  default to AND (photos containing ALL of these people together), with an OR
  toggle (photos with ANY of them) and an "only these people" option (photos
  where no OTHER detected person appears).
- **Place search:** photos carry their EXIF GPS coordinates; at index time the
  indexer resolves them to a place name OFFLINE (bundled gazetteer such as a
  GeoNames city dump compiled into the package - no outbound calls, ever) and
  also keeps IPTC/XMP location text fields when present. Place becomes a search
  chip that combines freely with name chips ("דוד" + "ים המלח"). Optional:
  pick-on-map + radius search.
- **Date range:** companion filter on EXIF DateTimeOriginal (cheap, same index
  pass); combines with name/place chips.
- **Manual location tagging:** for photos with no GPS, the owner can set a place
  on one photo or a selection. Entry methods (all offline, undoable, marked as
  manual vs camera GPS):
  1. **Typed place name** - autocomplete from the bundled offline gazetteer
     (GeoNames city-level dump: ~15-25 MB, worldwide cities + places; resolves to
     coordinates). This is the v1 baseline.
  2. **Direct GPS entry** - lat/lon in tolerant formats: decimal
     (32.0853, 34.7818), DMS (32°5'7"N 34°46'54"E), mixed; parsed and validated
     in the UI.
  3. **Street addresses** - OFFLINE street geocoding needs bundled street data
     (OpenStreetMap-derived index). Tradeoff to flag: a worldwide offline
     geocoder is far too heavy for this device; a scoped ISRAEL-ONLY
     street/place index (OSM Israel extract, prebuilt offline geocoder index,
     est. hundreds of MB) is feasible as an OPTIONAL downloadable add-on, sized
     in milestone 5. If not taken, addresses fall back to methods 1-2
     (nearest place name + manual GPS fine-tuning).
  4. **Pin-drop on a map (first-class, the primary manual method):** pan/zoom a
     fully OFFLINE map and drop the pin where the photo was taken; single photo
     or selection. Map stack: bundled vector tiles (MVT) rendered by a locally
     bundled MapLibre GL build; the app serves tiles itself straight from
     mbtiles files (SQLite) - no tile server, no external tiles, no CDN, no
     outbound calls. Licence: OSM data is ODbL, OpenMapTiles CC-BY, GeoNames
     CC-BY - attribution shown on the map and in the app About page, notices
     shipped in the repo.

     **Map packs (his disks, his choice):**
     - Always bundled: a coarse low-zoom WORLD base map (est. ~50 MB) - every
       place on earth is pinnable at country/city level out of the box.
     - Default detailed pack: Israel (est. 150-250 MB mbtiles).
     - **Optional regional packs:** per-country/region mbtiles (OpenMapTiles
       extracts; tens of MB for small countries up to several GB for large
       ones - a full planet pack is tens of GB, so it is pack-by-pack, not
       all-or-nothing). The owner adds packs for the regions the family has
       been to; detail level per pack is selectable (max zoom). Storage is
       limited only by his disks.
     - **Getting packs, two ways, both explicit:** (a) upload an mbtiles file
       through the web UI (he downloads it wherever he likes and supplies the
       file - the app stays 100% no-outbound); (b) a one-time download he
       explicitly triggers in the UI (shows source URL + size, asks
       confirmation) - outbound is OFF by default and this is the single
       owner-initiated exception, never background traffic.
     - Sizes per region are estimates, validated in milestone 5.
  Written to the sidecar XMP (GPS coordinates + IPTC/XMP location text fields)
  when write-back is on; originals never touched.
- People page shows one card per cluster with a grid of sample faces (largest /
  sharpest first), face count and a photo count.
- Tap a card -> name field with autocomplete from existing names (typing "דוד"
  or "david" both match the same person); confirm assigns the name to the cluster.
- Cluster hygiene actions on every card: **merge** (two clusters are the same
  person -> pick target), **split / "not this person"** (remove wrong faces from a
  cluster; they re-cluster), **"not a face"** (false positive detection -> excluded
  from people, kept in an ignore list so it never resurfaces).
- **Irrelevant person:** a whole cluster/person can be ignored ("not relevant to
  us") -> hidden from the People page, restorable from a hidden list (undoable).
  An optional **purge** also deletes their stored data (face rows, thumbnails,
  embeddings) - purge is the ONE irreversible action: it needs an explicit
  typed confirmation, is excluded from undo, and says so in the UI.
- **Per-photo correction:** on any single photo, one face can be reassigned to
  another person or to a new person (fixes a wrong identification without touching
  the rest of the cluster).
- **Manual face marking:** the owner can draw a box around a face the detector
  missed, on any photo, and name it. The embedding is computed from the boxed
  region and the face joins that person's cluster like any detected face (it also
  feeds future recognition). Manual faces are written to the sidecar as MWG
  regions like detected ones.
- Every action except purge is undoable from an activity log.
- **Sidecar consistency:** when metadata write-back is enabled, every action above
  (merge, split, ignore, purge, reassign) updates the affected photos' sidecars so
  names outside the app always match the DB. Purged/ignored people are removed
  from sidecars on the next write pass.

**Name format:**
- Free-text display name, Unicode-safe end to end (Hebrew names in the DB, the UI
  and the XMP sidecar - XMP is UTF-8, so "דוד כהן" is valid).
- Optional second "sort/search name" (e.g. latin spelling) for autocomplete and
  sorting; defaults to the display name.
- XMP mapping: display name -> `Iptc4xmpExt:PersonInImage` + MWG
  `RegionPersonDisplayName`; keywords get the name too (helps text search in
  other tools).

### Face-data portability (export/import)
Move ONLY the face data (no photos) to another device that holds a subset of the
same photo library (e.g. a backup NAS), and let it attach the data to its copies.

- **Bundle:** versioned schema (semver field, forward-compatible readers);
  contains per person: names (display + sort), ignore/purge states; per face:
  photo reference, face box, person link, OPTIONAL embedding (off by default -
  smallest/safest bundle; on when the target should skip re-recognition);
  per photo: content hash, EXIF date, GPS/manual locations. No photo pixels,
  no thumbnails.
- **Encryption:** optional passphrase encryption (e.g. age/XChaCha20-Poly1305)
  - recommended ON, this is biometric data; bundle permissions 600.
- **Matching on import:** primary key is the photo's **content hash**
  (SHA-256 of the file) - robust to renames and moves. Fallback: perceptual
  hash (pHash) for re-encoded/resized copies - always clearly LABELLED as
  approximate and shown for confirmation before attaching.
- **Pending records:** faces whose photo is not (yet) on the target stay in a
  pending queue; they attach automatically when a matching photo appears in a
  later scan.
- **Conflicts:** target-side existing data wins by default; merge rules per
  field (names merge, boxes add, newer manual correction wins); every decision
  logged.
- **Dry-run first:** import shows exactly what would attach/merge/pend before
  writing anything; undoable via the activity log (purge rules still apply).
- Fully offline: bundles move via the UI download/upload or a share; no
  outbound calls.

### Privacy (hard requirements)
- All processing local; the app makes no outbound network connections (enforced in
  code review; the UI bundles every asset).
- Repo and CI contain **synthetic faces only** (generated/test-pattern images).
  No real photos, no real embeddings, no personal data in git history.
- **Embeddings are biometric data:** the face DB lives in the app data dir with
  restrictive permissions (dir 700, files 600, app user only); the UI has a full
  **wipe** option (delete ALL app data - faces AND stored locations - keep
  photos); CI includes a no-outbound check (test asserting the app opens no
  external network connections). SQLite runs with secure_delete=ON and the app
  VACUUMs after purge/wipe so deleted embeddings are really gone. The activity
  log and the self-test report contain NO person names and NO biometric data -
  IDs, counts and timings only.
- Public repo by default, after a private-data scan, per standing convention.

## 5. Engineering standards (house rules applied)

- English code/README; UI strings Hebrew + English.
- Issue + PR per change, full PR/commit bodies, squash merge.
- GitHub Actions CI: build in the Bullseye container, unit + functional tests,
  `ruff`-equivalent linting for scripts, produce the `.bin` as a build artifact.
- ≥ 80% test coverage floor; failing tests → issue, documented.
- Semver + release notes for every release; README badges, LICENSE, SECURITY.md,
  Dependabot, architecture diagram (this doc, kept current).
- Web UI gets the full site treatment: favicon, security headers/CSP,
  accessibility, mobile version, e2e tests (Chrome + WebKit), security.txt.

## 6. Milestones

1. **Bootstrap** — repo, this design doc, skeleton apkg ("hello world" installable
   `.bin` built by CI), issue #1.
2. **Benchmark spike** — ncnn + SCRFD + MobileFaceNet built WITHOUT AVX code
   paths (baseline x86-64, AVX dispatch disabled) so container numbers are not
   inflated by host AVX; measure per-photo latency INCLUDING image decode
   (libjpeg-turbo), per-face embedding latency and RAM. Also measure the bundled
   GeoNames gazetteer's size and in-RAM lookup cost. Self-test images must match
   REAL photo resolutions (e.g. 12 MP JPEGs, synthetically generated) so decode
   cost is not understated. Container numbers are estimates only - the on-device
   self-test report is the authority for go/no-go.
3. **Indexer** — share scanning, detection, embedding, SQLite schema,
   incremental rescan, and the resource cap (below).
4. **Clustering & naming** — people grouping, merge/rename, confidence thresholds.
5. **Web UI** — mobile-first UI + local API, including cluster naming.
6. **Metadata write-back** — Exiv2 integration, sidecar .xmp only (owner's choice),
   write logging, round-trip test.
7. **Export/download (part of the web UI)** — merge photo + sidecar into ONE
   self-contained file for handoff, straight from the app's web page. Selection:
   multi-select in the photo grid (tap-to-select, select-all-filtered, per person /
   per folder / manual selection). Delivery: one-by-one downloads, or a bulk ZIP
   with each photo's XMP embedded per file; large batches can instead be written
   to a chosen export folder. Option: also write person names into the Description
   field (visible as text in Google Photos and similar tools). Originals on the
   NAS are never touched. NAS-safe limits: ZIP is streamed (no full archive in
   RAM), memory-capped, with progress shown, cancel, and resumable batches; an
   estimated total size is shown before starting. All batches logged.
8. **Face-data portability (export/import)** — see "Face-data portability" section.
9. **Hardening & release** — packaging polish, docs, v1.0.0 release notes.

## 7. Open questions

1. ~~Device access~~ — RESOLVED: dashboard `.bin` install is the primary path;
   the owner installs test builds himself. SSH only if ever specifically needed
   (explicit request + vault link).
   ~~Initial include/exclude folders~~ — RESOLVED: photos live across all folders;
   discovery covers all shares, default stays nothing-scanned until he picks.
2. RAM: stock 4 GB is supported; does he want the optional 16 GB upgrade?
3. UI language default: Hebrew-first or English-first?

## 8. Sources

Hardware / OS:
- Intel ARK, Pentium N3710: https://www.intel.com/content/www/us/en/products/sku/91828/
- WD PR4100 spec sheet (CPU/RAM): https://docs.rs-online.com/580d/A700000009426450.pdf
- WD PR4100 user manual: https://documents.westerndigital.com/content/dam/doc-library/en_us/assets/public/wd/product/nas/my_cloud/pro_pr_4100/user-manual-my-cloud-pro-series-pr4100.pdf
- WD KB: RAM upgradeable to 16 GB; OS5 indexing/media services: https://support-en.wd.com/app/answers/detailweb/a_id/29819/
- WD KB: Twonky on OS5: https://support-en.wd.com/app/answers/detailweb/a_id/30285/

App packaging (community):
- WD NAS App Builder (apkg template, Bullseye build, PR4100-tested): https://github.com/jessedp/WD-NAS-App-Builder
- OS5 SDK status threads: https://community.wd.com/t/where-can-i-find-documentation-for-the-os5-api-or-an-sdk/278796 , https://community.wd.com/t/photo-index-db/298167 , https://community.wd.com/t/hide-pictures-or-videos-in-os-5-my-cloud-app/259853

ML stack:
- InsightFace SCRFD: https://github.com/deepinsight/insightface/blob/master/detection/scrfd/README.md
- MobileFaceNets: https://arxiv.org/abs/1804.07573
- ncnn: https://github.com/Tencent/ncnn

Metadata:
- Exiv2: https://exiv2.org/
- Immich XMP sidecar naming: https://docs.immich.app/ (XMP Sidecars page); https://github.com/immich-app/immich/pull/7813
- Google Photos metadata limits: https://support.google.com/photos/thread/93983534 ; API policy: https://developers.google.com/photos ; API people r/w: https://stackoverflow.com/questions/63128115
- Offline gazetteer: GeoNames dumps https://download.geonames.org/export/dump/ ; Israel street data (optional): OpenStreetMap Israel extract https://download.geofabrik.de/asia/israel-and-palestine.html
