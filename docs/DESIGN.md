# WD Face Recognition — Design Doc
**On-device face recognition for WD My Cloud PR4100, packaged as a native My Cloud OS 5 app**

Status: draft v0.41 · 2026-10-27 · Owner: the owner

---

## 1. Goal

A WD My Cloud OS 5 app for the PR4100 that scans **all photos on the device**,
detects faces, clusters similar faces into people, and serves a web page on the NAS
where the owner names each cluster — then can optionally write the person's name into
the photo's XMP metadata (section 4.3). **100% on-device**: no cloud calls, no
telemetry, and nothing is sent anywhere by the app itself. Installable the way any WD app
is installed: Settings → Apps → Install an app manually.

## 2. Target hardware and OS (grounded - see Sources)

| Fact | Value | Source |
|---|---|---|
| CPU | Intel Pentium N3710, 4 cores / 4 threads, 1.6 GHz (burst 2.56 GHz), Braswell (Airmont), 14 nm, 6 W TDP | Intel ARK; WD product spec sheet |
| ISA ceiling | SSE4.2 — **no AVX/AVX2** (Airmont core) | Intel ARK (Instruction Set Extensions) |
| RAM | 4 GB DDR3L (2×2 GB), upgradeable to 16 GB | WD support / spec sheet |
| OS | My Cloud OS 5 — stripped Debian 11 (Bullseye), x86_64, no package manager, outdated system libraries. Target = LATEST OS5 (owner's device is up to date; latest grounded release: firmware 5.33.102, 2026-03-24 - re-check before each release) | jessedp/WD-NAS-App-Builder; WD release notes |
| App format | `.bin` apkg bundle: `apkg.rc` + lifecycle scripts (`install.sh`, `init.sh`, `start.sh`, `stop.sh`, `remove.sh`), installed via the web dashboard, no SSH needed | WD NAS App Builder template |
| Build env | Debian 11 (Bullseye) Docker container to match the NAS; binaries must be statically linked or bundled | WD NAS App Builder |
| Official SDK | WD's developer portal/SDK appears discontinued; community packaging conventions are the de-facto standard | community.wd.com threads |

**Caveat:** OS5 internals and app-format details rest on community sources
(WD's official developer docs are offline); they are validated on the real
device in milestones 1-3 before we rely on them.

**Key consequence:** no AVX and 4 GB RAM rule out the usual Python + ONNX Runtime +
InsightFace stack (heavy RAM, prebuilt wheels tuned for AVX). The inference engine must
work well on SSE4.2-only silicon.

## 2a. Performance-first principle (owner rule for ALL contributors)

The target is a weak device (N3710, 4 GB). Efficiency is a feature, and it is
every contributor's job (Instinct, Claude, Tab):

- **Budgets per component** (validated in M2, enforced in CI): indexer worker
  <= 1.5 GB RSS and <= 80% CPU under load; web UI backend <= 256 MB RSS idle;
  app startup to ready <= 30 s on-device; UI pages <= 1 MB initial payload
  (map/tile views included - tiles stream, no bulk prefetch).
- **Lightweight models only:** the smallest model that meets the accuracy bar
  (SCRFD-0.5GF class detector, MobileFaceNet-class recognizer), quantized
  (int8) where accuracy holds; any model upgrade must re-justify its budget.
- **Bounded everything:** streaming everywhere (ZIP export, tile serving, image
  decode), fixed-size queues, no whole-library structures in RAM; incremental
  scanning by mtime delta, never a blind full re-walk. Face indexing caps one
  photo at 50MP: worst case is a progressive JPEG whose decode peaks near
  450MB, which fits the indexer worker's 1.5GB RSS budget with headroom for
  the OS, Samba and the web UI on the 4GB device (independent review #45).
- **I/O discipline:** batched SQLite writes, WAL, read-ahead-friendly sequential
  file access, ionice idle class.
- **Regression gates:** CI runs the benchmark suite with THRESHOLDS - a PR that
  regresses latency, RSS or payload beyond the budget fails CI. The on-device
  self-test remains the final authority.

## 2b. Dependency-minimalism principle (owner rule, 2026-10-27)

Minimize libraries that are not already on the device:

- **Nothing is fetched at runtime, ever.** The `.bin` carries everything the
  app needs; the device may be offline and stays that way.
- **Prefer what OS5 already ships** where its version is usable; bundle only
  what the OS lacks or versions too old to trust.
- **Minimal static set, each one justified:** every bundled library needs a
  line in `DEPENDENCIES.md`: name, pinned version + source, licence, why it is
  needed, unpacked size, and the audit note (known-CVE check at pin time +
  Dependabot/renovate tracking after). No transitive bloat: a dependency that
  pulls a tree needs extra justification.
- **Current bundle under review (must each earn its place):**
  - ncnn - inference runtime. NEEDED (no smaller maintained option for SCRFD
    + MobileFaceNet on SSE4.2).
  - Exiv2 - XMP read/write. NEEDED (XMP correctness by hand is error-prone);
    evaluate trimming to metadata-only build.
  - libjpeg-turbo - JPEG decode. NEEDED; check if the OS's libjpeg is usable
    first, bundle only if not.
  - SQLite (C amalgamation) - index DB. NEEDED, tiny.
  - Rust web stack (axum/tokio or smaller) - REVIEW: pick the smallest HTTP
    stack that serves the UI; a micro-framework beats a full async stack if it
    meets the API shape.
  - MapLibre GL (UI bundle) - NEEDED for the pin-drop map; assets bundled, no
    CDN.
  - age-format crypto for bundle encryption - NEEDED only for M8; small,
    audited implementation.
  - GeoNames gazetteer data - data, not code; sized in M2.
- Adding ANY new dependency requires naming it in the PR body with its
  justification; CI fails on an undeclared addition (lockfile diff check).
- **Baseline verified from real data:** the on-device self-test records the
  exact firmware version, kernel, glibc version, CPU flags (`/proc/cpuinfo`),
  RAM and installed system libraries (see DEVICE-TEST) - the Bullseye/static
  assumptions are confirmed or corrected from THAT data, not guessed.
- **Robustness against host drift (owner rule):** a library must never
  suddenly need something the device lacks. Binaries are FULLY STATIC
  (preferred) or FULLY BUNDLED - every shared lib shipped inside the app with
  `rpath=$ORIGIN`, relying on host glibc/system libs only within what OS5
  (Bullseye) guarantees; builds happen in the Bullseye container so the ABI
  baseline matches the device. All versions pinned. **CI gate:** `readelf -d` /
  `ldd` on every shipped binary, failed if a needed library is not in an
  explicit allowlist (or not resolved inside the package). **On-device
  self-check:** at startup the app verifies its own linkage and reports any
  missing library/symbol in clear language on the setup screen (and in the
  self-test report) instead of dying silently.

## 2c. Installation-simplicity principle (owner rule, 2026-10-27)

Installing and living with the app must be simple for a non-technical owner:

- **One self-contained `.bin`** installed from the WD dashboard (Apps ->
  Install an app manually). No SSH, no CLI, no extra packages, no runtime
  downloads (see 2b).
- **First-run wizard** (part of the first-run claim in the access-control
  section): 1) enter the WD dashboard admin credentials (owner check), 2)
  create the app password, 3) choose folders to scan (default: nothing),
  4) done - scanning starts on schedule. Sensible defaults for everything
  else; advanced settings available but never required.
- **Upgrade path:** install the new `.bin` over the old via the dashboard;
  data (DB, names, sidecars, settings) is kept; schema migrations run
  automatically and are tested in CI.
- **Uninstall path / retention contract:** dashboard remove keeps BOTH the
  face DB and the reports share (and the owner's settings), so an
  uninstall/reinstall cycle loses nothing; the explicit "remove data too"
  option deletes all three. Verified on-device in DEVICE-TEST.
- **The DEVICE-TEST run sheet covers exactly this flow** (install / first-run
  wizard / start / stop / upgrade / remove) so simplicity is verified on the
  real device, not assumed.

## 3. Feasibility verdict

**Feasible — as a background batch indexer, not a realtime service.**

- Detection: SCRFD-0.5GF / 2.5GF (InsightFace's lightweight detector family).
- Recognition: MobileFaceNet-class ArcFace model (≤ 4 MB, ~1M params, 99.5% LFW),
  e.g. the `w600k_mbf`/`buffalo_sc` recognition model.
- **Model provenance (required before packaging):** family names are not
  artifacts. M2 must pin the EXACT weight files in DEPENDENCIES.md: source URL,
  revision, SHA-256 checksum, input shape, measured size, and the
  redistribution licence (verified, not assumed - some published face-model
  weights carry non-commercial terms; if the chosen weights do, they are
  replaced before any packaging).
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
|    - union-find over cosine-similar embedding pairs (0.5 cut); full   |
|      recluster reuses stored embeddings and keeps ids/names by        |
|      global max-overlap claims: larger OR equal overlap from any      |
|      live cluster can win a contested group (ties: group order,       |
|      then lowest cluster id; see the recluster rule below)            |
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

### Recluster engine and cluster identity (0.3.0, issues #50/#71)

`wdf_faces cluster` rebuilds all cluster assignments from STORED embeddings
(no model run): union-find over pairs with cosine similarity above the
threshold (default 0.5, demo-calibrated for MobileFaceNet), skipping pairs
with missing embeddings or mismatched dimensions (version-skew guard).
Groups come out deterministically ordered: largest first, equal sizes by
smallest member face id.

Identity preservation goes through `Db::replace_clusters` under one
transaction. The claim rule is GLOBAL max-overlap: every (new group, live
cluster) pair is scored by shared faces and pairs claim best-first -
overlap descending, ties by caller group order, then lowest cluster id.
A split cluster's largest fragment keeps its id and name UNLESS another
live cluster claims that group with a larger OR EQUAL overlap - the
competitor need not be wholly absorbed, and an equal-overlap claim
decided by group order or lower cluster id can also take the group. The
split cluster's identity then continues on its largest UNCLAIMED
fragment; if every fragment was claimed by other live clusters, the
deletion pass drops the split cluster entirely. Rationale for choosing
this over per-cluster largest-fragment priority: no cluster's identity
is destroyed by a split alone. Determinism holds for a FIXED group
order: `wfi::recluster` sorts groups (largest first, smallest face id
tiebreak) before calling in, but `Db::replace_clusters` itself takes
the caller's order. Unclaimed live clusters are dropped; merged-away
rows are audit history and are superseded by a fresh recluster.

### Web UI access control
The UI can view private photos and mutate data (purge, sidecar writes) - "LAN only"
is not enough:

- **Authentication required for everything** (viewing included). v1: app-local
  login (owner-set password, argon2-hashed), session cookie (HttpOnly,
  SameSite=Strict, Secure when served over HTTPS). No anonymous mode.
- **First-run claim, owner-bound:** on first launch the UI demands password
  creation before anything else works - until then every route shows only the
  setup screen. Enrollment additionally requires proof of being the NAS owner:
  the wizard asks for the WD dashboard ADMIN credentials (validated LOCALLY
  against the OS on the box, never stored) - a random LAN device cannot enroll.
  (Fallback if OS-credential validation proves unavailable on OS5: a one-time
  setup token written to a path readable only via the authenticated WD
  dashboard file manager.)
- **Transport: HTTPS by default.** The app generates a self-signed certificate
  at install and serves :8443. The certificate's SHA-256 fingerprint is
  delivered OUT OF BAND: written at install to
  `/shares/facerecog-reports/fingerprint.txt`, readable only via the
  authenticated WD dashboard file manager. The owner compares the browser's
  fingerprint against THAT file before entering any credentials - a
  fingerprint shown by the setup page itself proves nothing (an on-path
  attacker could show a matching one). Plain HTTP on :8088
  is a fallback the owner must explicitly enable, with a warning that
  credentials cross the LAN in cleartext. Enrollment (which sends the WD
  ADMIN credentials) is REFUSED over plain HTTP - it requires HTTPS, or the
  owner's explicit trusted-LAN confirmation before HTTP enrollment is
  allowed at all.
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
- Binaries staged into apkg/bin (issue #56): wdf-worker, wdf-service,
  wdf_demo, and wdf_faces (the people pages' CLI backend, fully static
  like wdf_demo; SQLite comes from the pinned amalgamation compiled
  in, so only ncnn + libjpeg are external). wdf_indexer joins when the
  daemon side of #40 lands.
- Linking per section 2b: FULLY STATIC preferred; otherwise fully bundled
  (every shared lib inside the package, rpath=$ORIGIN); host reliance limited
  to what OS5 (Bullseye) guarantees.
- **Install path (primary, by user decision): dashboard only.** The owner installs
  the `.bin` via the OS 5 web dashboard (Settings → Apps → Install an app manually).
  SSH is NOT part of the normal flow - development, CI and on-device testing all work
  without it (the owner installs test builds himself). If SSH access is ever needed,
  it will be requested explicitly with the exact reason, and credentials only via a
  vault link, never in chat.
- Uninstall removes binaries; the face DB, settings and reports share are kept
  (explicit "remove data too" option deletes all three) - see 2c.

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
  - Same-stem collisions: ANY distinct files sharing a basename
    (`IMG_1234.CR2`+`IMG_1234.JPG`, but also `.CR2`+`.DNG`, `.JPG`+`.TIF`,
    case variants) collide on `photo.xmp`. Rule: whenever a basename maps to
    more than one distinct file, the app writes the full-filename form
    (`IMG_1234.CR2.xmp`, ...) for each of them; collision-free basenames use
    `photo.xmp`. Collision state is RE-CHECKED under a per-directory lock
    immediately before the atomic commit; if the directory changed or the
    state cannot be confirmed, the full-filename form is used.
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
     - **Getting packs: upload only.** The owner supplies mbtiles files through
       the web UI (he downloads them wherever he likes, on any device, and
       uploads). The app has NO download path - "no outbound, ever" stays true
       everywhere (privacy rules, SECURITY.md, and the CI no-outbound test).
     - **Uploaded mbtiles = untrusted input:** schema/validity check (must be a
       well-formed MVT mbtiles), size limit, opened READ-ONLY, and the upload
       endpoint requires auth + CSRF token. Rejected files are deleted, not
       kept.
     - Sizes per region are estimates, validated in milestone 5.
  Written to the sidecar XMP (GPS coordinates + IPTC/XMP location text fields)
  when write-back is on; originals never touched.
- People page shows one card per cluster with a grid of sample faces (largest /
  sharpest first), face count and a photo count.
  (0.3.0 status: list page with cover crop, name and face count, rename and
  merge actions, and a per-person page with every face in-page are
  implemented; split/ignore/purge and sample-grid ordering are still open.)
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
- **Purge durability:** a purge writes a durable PURGE MANIFEST (affected photos
  + fields to strip) BEFORE deleting any records; the sidecar cleanup worker
  processes the manifest and only then are the DB records deleted; the manifest
  survives restarts and is retired only when every cleanup write has succeeded.
  A crash mid-purge therefore cannot strand stale names in sidecars.

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
- **Encryption: REQUIRED on export** for every bundle - this is biometric data
  and file permissions (600) protect only the NAS copy, not a downloaded one.
  Format: the **age format with a passphrase recipient** (ChaCha20-Poly1305
  payload, scrypt-wrapped key) - one precise contract, age-compatible tools
  can also open the bundle off-device. The import side accepts only encrypted
  bundles.
- **Matching on import:** primary key is the photo's **content hash**
  (SHA-256 of the file) - robust to renames and moves. Fallback: perceptual
  hash (pHash) for re-encoded/resized copies - clearly LABELLED approximate and
  NEVER auto-attached: every pHash match waits for owner confirmation.
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
- **Imported bundle = untrusted input:** schema version check, strict field
  validation, size limits (compressed AND decompressed size, plus entry-count
  caps), parsed read-only in a sandboxed reader; the upload endpoint requires
  auth + CSRF token. A malformed bundle fails the dry-run with a clear error -
  it never reaches the DB. Export/download endpoints carry the same auth + CSRF
  requirements.

### Privacy (hard requirements)
- All processing local; the app makes NO outbound network connections, ever -
  no exceptions (map packs and face-data bundles are upload-only). Enforced by
  the CI no-outbound test; the UI bundles every asset.
- **Owner-authorized exports are the only way data leaves the box, and they are
  the owner's own act, not app exfiltration:** photo downloads (M7) and
  face-data bundles (M8) move only through the authenticated UI (auth + CSRF),
  are logged in the activity log, and bundles are passphrase-encrypted (see
  below). The app never transmits anything on its own.
- Repo and CI contain **synthetic faces only** (generated/test-pattern images).
  No real photos, no real embeddings, no personal data in git history.
- **Embeddings are biometric data:** the face DB lives in the app data dir with
  restrictive permissions (dir 700, files 600, app user only); the UI has a full
  **wipe** option (delete ALL app data - faces AND stored locations - keep
  photos); CI includes a no-outbound check (test asserting the app opens no
  external network connections). SQLite runs with secure_delete=ON and the app
  VACUUMs after purge/wipe to remove deleted embeddings FROM SQLITE STORAGE;
  face thumbnails, WAL/journal and temp files are deleted separately
  (overwrite-then-delete where the filesystem allows); NAS snapshots and RAID
  replicas are outside the app's reach and documented as such. The activity
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

## 5a. ADR-001: Implementation language

**Status: ACCEPTED (provisional, 2026-10-27, owner-approved after
clarification) - C++ native worker + Rust service layer. Go was evaluated
and DROPPED by the owner ("forget Go"; the inference libraries force FFI
anyway, so Go's static-build advantage doesn't pay off here - see g2).
Confirmed or revised by the M2 benchmark results (#3) and the language
shootout (#14).**

Context: N3710 (4C/4T, SSE4.2, no AVX), 4 GB RAM, OS5 = stripped Debian 11,
apps ship as self-contained packages. Question raised by the owner: "maybe C or
C++ would be more right". Honest evaluation per layer. All numbers marked
(est.) are estimates until M2 measures them.

**(a) Inference runtime:** the heavy math lives in ncnn (C++) regardless of the
app language - Python/Go/Rust would all bind into the same C++ core. Language
choice changes ORCHESTRATION overhead only, which is small next to inference
time. No winner here; this layer is C++ by necessity.

**(b) Indexer / scanner / hashing / EXIF / XMP:** Exiv2 and libjpeg-turbo are
C/C++; pHash and SHA-256 are native in any language. Python is the worst fit:
interpreter baseline ~30-60 MB RSS (est.) before any work, slow startup, and
the GIL fights multi-threaded pipelines. C++ is native here; Rust and Go bind
via FFI with thin overhead.

**(c) Web backend:** Go: single static binary, excellent stdlib HTTP, trivial
cross-compile - but CGO_ENABLED=0 static builds conflict with FFI to ncnn/Exiv2
(cgo needed), and musl+cgo static linking is painful. Rust (axum): smallest
RSS (est. 5-20 MB), no GC, same FFI caveat but the ecosystem handles it.
C++ embedded HTTP (crow/drogon): fewest moving parts in one binary, highest
defect risk and dev effort. Python (FastAPI): easiest to write, worst RSS
(est. 100+ MB with deps) and slowest startup against the 30 s budget.

**(d) OS5 packaging:** the convention is statically linked binaries built in
the Bullseye container. C++ links ncnn/Exiv2 directly - the natural fit.
Rust-musl static is well-trodden. Go static is trivial ONLY without cgo.
Python needs a bundled interpreter (PyInstaller-style) - biggest artifact,
most fragile.

**(e) Memory/startup:** C++ ~ Rust (few MB RSS, ms startup) > Go (runtime +
GC, est. 15-50 MB) >> Python (est. 60-150 MB with libs).

**(f) Safety on untrusted input:** the app parses owner-supplied mbtiles,
face-data bundles, existing sidecars - and ordinary photos, which are also
untrusted bytes (malformed JPEG/XMP is a classic CVE vector in C libraries).
Rust's memory safety is the strongest answer for the parsers we WRITE. But
the C libraries we must use (ncnn, Exiv2, libjpeg-turbo) stay C/C++ in every
option - so the real control is ISOLATION: the worker that touches photo
bytes runs sandboxed whatever language wraps it: an ISOLATED network
namespace (loopback-only or no interfaces at all) so no outbound connection
is possible, PLUS a seccomp filter denying socket()/connect() where feasible,
PLUS rlimits and read-only mounts. The no-outbound requirement is therefore
enforced on the worker even if a C parser is compromised; a CI/device check
proves it (the self-test has the worker attempt a connection that would
otherwise succeed - it must fail, see DEVICE-TEST).

**(g2) Go explicitly (owner asked "maybe Go, if it's supported"):**
Platform support: YES - Go targets linux/amd64, which the PR4100 (x86_64, OS5)
runs; a Go toolchain package for OS5 even exists in the community builder repo
(listed as untested, but that's the toolchain, not the runtime - compiled Go
binaries need no runtime). Static binaries are Go's signature feature:
`CGO_ENABLED=0 go build` yields a fully static, dependency-free binary - the
easiest packaging story of all candidates.
The catches for THIS app:
- **Face inference has no pure-Go path:** ncnn and ONNX Runtime are C/C++;
  their Go bindings (onnxruntime_go, gocv/OpenCV) all need cgo, and cgo
  reintroduces exactly the static-linking pain (musl+cgo) that Go otherwise
  avoids.
- **SQLite:** fine - modernc.org/sqlite is pure Go (no cgo); mattn/go-sqlite3
  needs cgo but is avoidable.
- **GC on 4 GB:** manageable - GOMEMLIMIT/GOGC tuning keeps RSS in budget
  (est. 15-50 MB typical for this workload), but it's one more thing to tune
  that Rust/C++ don't have.
Verdict: Go is SUPPORTED on the platform, but since the inference core
forces cgo anyway, Go's main advantage (painless static builds) is partly
lost - **the owner dropped Go (2026-10-27): no fallback, no Go deliverable.
It appears in the #14 shootout only if keeping it as a reference row is
trivial.**

**(g) Maintainability (Instinct/Claude/Tab):** Python > Go > Rust > C++ for
iteration speed. The codebase is small; AI contributors handle Rust/C++ fine,
but C++ debug cycles are the slowest.

**Recommendation (draft):** split by concern -
1. **Native C++ worker** for the hot paths (decode/detect/embed/hash/EXIF),
   statically linked against ncnn/Exiv2/libjpeg-turbo, built in the Bullseye
   container, run as a sandboxed subprocess. This is also the packaging
   convention's home turf.
2. **Rust service layer** for everything else: web backend, UI API, DB
   orchestration, and ALL parsers we write ourselves (mbtiles, bundle, XMP
   merge logic) - memory safety exactly where untrusted input enters, tiny
   RSS, static musl build.
3. **Python ruled out for on-device daemons** (RSS/startup budgets); still
   fine for CI tooling and test harnesses.
Fallback: if the two-language build proves too costly in M1, collapse the
service layer into the C++ worker with an embedded HTTP server and accept
the extra review burden - not the reverse (no Python/Go service on-device).

**What M2 settles:** whether the sandboxed-worker overhead and FFI boundaries
cost anything measurable; whether int8 ncnn meets the latency budget; actual
RSS per component vs the 2a budgets. Language choice does NOT change
inference speed materially - it changes RSS, startup, packaging and safety.

**Language shootout (owner: "run tests to find what is most efficient"):**
same small workloads implemented in C++, Rust and Python (Go only as a
trivial-to-keep reference row, per owner):
1. SHA-256 hashing of a 12 MP file stream
2. EXIF/XMP parse of a synthetic JPEG
3. JPEG decode (libjpeg-turbo path each language would really use)
4. HTTP + JSON request handling (the UI API shape)
5. SQLite batch writes (modernc/pure where applicable)
6. ncnn inference call overhead (FFI cost per call, tiny model)
Measured per runtime: RSS (peak/idle), cold startup, throughput - inside the
Bullseye container with AVX DISABLED and cgroup limits (4 GB RAM, 2 cores) to
approximate the device. Synthetic data only. All results LABELLED as container
estimates; the on-device self-test run (DEVICE-TEST) is the final authority.
Runs in GitHub Actions (free for this public repo).

## 6. Milestones

1. **Bootstrap** — repo, this design doc, skeleton apkg ("hello world" installable
   `.bin` built by CI), issue #1. Toolchain per ADR-001 (accepted provisional):
   C++ worker + Rust service, both STATIC builds inside the Debian 11
   (Bullseye) container (Rust via musl target); CI produces both binaries +
   the `.bin` package.
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
- OS5 release notes (live): https://os5releasenotes.mycloud.com/ ; firmware 5.33.102 announcement: https://community.wd.com/t/my-cloud-os-5-firmware-release-note-5-33-102/476697 ; OS5-supported models: https://support-en.wd.com/app/answers/detailweb/a_id/29230/
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
