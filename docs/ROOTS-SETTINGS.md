# Roots settings page (issue #102)

The owner sets which shares/folders the indexer scans by editing
`roots=` in `wfi.conf`. Today that means SMB file access; this page
moves it into the dashboard, next to the output-folder setting.

## Decisions (owner-approved: "בנה" - build it - phonemsg-01M62KC6RKS20DWV0ZWFNKZYMW, 03:11 Thu 29 Oct 2026)

- **A settings page, not the dashboard file manager.** The file
  manager edits bytes; the page can validate, normalize, and persist
  atomically, and it reuses the app's existing PIN+CSRF mutation
  contract.
- **The empty default stays.** Privacy first (#56): a fresh install
  scans nothing until the owner adds a root.

## UI

A "Scan roots" section on the app page: `index.php` is the registered
`AddonIndexPage` and already renders the settings section backed by
`apkg/web/settings.php`; roots join it there. A textarea, one absolute
path per line, prefilled from the live `wfi.conf`. Save goes through
the same mutation contract as `people.php`: PIN or remembered session,
per-session CSRF token, PRG so a refresh never repeats the write.

Load/save conversion: on load, the comma-separated `roots=` value is
split and shown one path per line; on save the validated lines are
joined back with commas (which is why a comma inside a path is
rejected) and capped at 16 roots.

## Validation (every line)

1. Absolute path under `/shares/`.
2. No `..` component.
3. Symlink components only at the share level
   (`wdf_path_symlink_components_ok`), and the resolved path stays
   under the shares' real roots
   (`wdf_real_path_under_roots` + `wdf_shares_real_roots`).
4. Must EXIST and be a real directory. Unlike the output folder (a
   future state the app creates), a scan root that does not exist is
   a typo.
5. A share ROOT is allowed. The subfolder-only rule for the output
   folder exists because runs replace its contents; scanning a share
   root is the normal case and wipes nothing.
6. No commas in a path: `roots=` is comma-separated, so a comma
   would silently split one root into two.
7. Dedupe; at most 16 roots; at most 512 bytes per path.
8. The empty list is valid - it is the shipped default (see "Empty
   roots contract" below).
9. No overlapping roots. After each line is canonicalized (the real
   path the symlink/`.`-resolution helpers produce), a root that is an
   ancestor or descendant of another listed root is rejected: the
   indexer would walk the nested tree twice per scan, and the
   duplicate observation paths would confuse the per-root missing
   sweep. Exact duplicates collapse under dedupe (rule 7) before this
   check runs.

## Persistence

Rewrite only the `roots=` line; every other line of `wfi.conf` is
preserved verbatim, and a missing line is appended.

`wdf_wfi_config_path()` points next to the app - and on device
`init.sh` makes that path a SYMLINK to the real file in the persistent
data path. The save therefore resolves the symlink first and writes
beside the TARGET: `wfi.conf.tmp.<pid>` in the target's directory,
`fflush` + `fsync`, then `rename()` over the target. Renaming over the
symlink path itself would silently replace the link with a regular
file and detach the app from its persistent config. A test pins the
symlink surviving a save. `init.sh` already uses the temp + `mv` half
of this pattern (pinned by `apkg/tests/init_conf_test.sh`); the page
adds the `fsync` before the rename.

## Empty roots contract

The empty list must be a SUCCESSFUL no-op FOR THE FILESYSTEM SCAN,
not an error: a fresh install ships with no roots (privacy first,
#56), and erroring on the shipped default would brick every new
install's first run. "No-op" scopes to the scan only - the DB work
below keeps running.
Shipped matrix (#108 + root removal, per CodeRabbit #103): only the
WALK is the no-op; DB work keeps running. `load_config` accepts zero
roots; `wdf_indexer` prints "no roots configured - nothing to scan",
skips the walk and exits 0 - but still opens the run's one shared job
and sweeps every stored root under it (the zero-root corner of the
removal rule below: clearing ALL roots flips every indexed photo to
`missing`, reversibly - see "Restore path" below); `wdf_faces` DB operations (`process`,
`cluster`, `gc`, people rename/merge) keep operating on the existing
index; read commands are unaffected. The
contract test pins BOTH halves end to end (CodeRabbit #103): on a
fresh install, empty `roots=` -> both binaries succeed, the DB gains
no photos, stderr carries no error; and with a SEEDED index (a photo,
a face, a person present), clearing roots still lets `process`,
`cluster`, `gc`, people rename/merge and all read commands operate on
the existing data - empty roots stops new scanning, it does not freeze
the index.

## Root removal semantics

`sweep_missing` used to run per CONFIGURED root, so a root deleted
from `roots=` was never walked and never swept: its photos stayed
`scanned` forever, and their faces kept feeding people identities -
stale clusters and missed missing-file sweeps (CodeRabbit #103).
Shipped: after sweeping each configured root, `wdf_indexer` also
sweeps every DISTINCT `root` stored in `photos` that is no longer
configured. Such a sweep needs no
walk, and only its photos NOT observed in the run's shared job flip
to `missing`: an overlapping configured root may have re-observed
some of them. Unchanged re-observations keep their stored root and
update only `last_seen_job` (exempt via `last_seen_job == J`);
changed ones adopt the current walk's root
(`observe()` sets `root = excluded.root`), leaving the stored root
entirely - both kinds escape the sweep. The rest flip to `missing`,
their faces stop feeding identity rebuilds, and re-adding the root
later brings them back through the restore path below.

Shipped as one job generation per RUN, shared by every configured
root and by the unconfigured-root sweeps (CodeRabbit #103):
`sweep_missing` marks photos whose `last_seen_job` differs from the
sweeping job, so per-root job ids would mark photos observed earlier
in the SAME run as missing. `scan_jobs.root` records the run's
comma-joined root list. The zero-root corner follows from the same rule: a run with
no configured roots still opens one job and sweeps every stored root
under it - clearing ALL roots flips every indexed photo to `missing`
(reversible; re-adding a root restores it).

Coverage gate on unconfigured sweeps (CodeRabbit #103, reviewer):
a configured root's sweep already runs only after a COMPLETE walk (a
depth-capped or unreadable walk is INCOMPLETE and skips the sweep).
An unconfigured stored root has no walk, and its sweep is exact for
complete coverage because re-observed photos are exempt: `observe()`
keys rows by path, so a file beneath the removed root that is still
walked under a surviving configured subroot is re-observed under the
run's job - unchanged, it keeps its stored `root` and updates only
`last_seen_job`; changed, it adopts the subroot as its `root` -
and either way sweeping the stored root under that job marks only
genuinely unobserved photos `missing`, never the re-observed ones. The one hazard is INCOMPLETE coverage: if an overlapping
configured root's walk was depth-capped or hit unreadable
directories, the files it failed to reach look unobserved and would
be wrongly swept. Rule: an unconfigured stored root overlapped by
configured roots (ancestor/descendant, canonicalized - the rule-9
relation) is swept under the run's shared job only when EVERY
overlapping configured root's walk completed this run; any incomplete
overlapping walk skips that sweep. Canonicalization resolves the
longest EXISTING prefix and appends the cleaned tail, so a
temporarily missing (unmounted) root and an existing symlinked
neighbour compare in one consistent form (CodeRabbit #112). The
"is this stored root still configured" check is EXACT TEXT, not
canonical: `sweep_missing` matches on the root column's text, so a
trailing-slash config edit must not skip the old rows (CodeRabbit
#112). Regression tests: (a) index under
`/shares/Photos`, narrow the config to `/shares/Photos/2020`, rescan -
photos under `2020` stay `scanned`, the rest flip to `missing`;
(b) the same setup with an unreadable `2020` subdir (incomplete walk)
flips nothing.

## Missing photos and identity data

A photo in `missing` state is excluded consistently from everything
identity-shaped (shipped, #102): embedding loads / cluster input
(`all_face_embeddings`), people counts and cover crops
(`people_list`), and person details (`person_faces`) all filter
`p.state != 'missing'`. The face ROWS stay - exclusion, not detachment
- and `replace_clusters` keeps both the membership of missing-photo
faces and any live cluster whose remaining faces sit on missing
photos, so a name survives an all-missing rebuild and a restored root
rejoins the SAME person through the normal overlap claim. `gc` still
counts missing photos' crops as referenced (nothing is unlinked while
a restore is possible). The root-removal tests verify the full cycle,
not just the state column: scan with the root, rescan without it
(photos `missing`, faces out of clustering and people, person intact),
re-add the root (photos and faces back).

## Restore path (reviewer #112)

A photo back from `missing` with IDENTICAL size+mtime is the same
file that was indexed: its processed face rows and their cluster
memberships are still valid. `observe()` therefore restores it WITHOUT
re-queueing (outcome "restored", counted separately in the scan
summary): the pre-`missing` state is recovered from
`scanned_at`/`fail_reason` (never processed -> `queued`, processed ->
`scanned`, failed -> `failed`) and only `last_seen_job` moves.
Re-queueing an unchanged return would re-run `replace_faces`, whose
fresh face rows carry `cluster_id NULL`; the next rebuild's overlap
claim - keyed by face row id - would find no face pointing at the
named cluster and the person would go anonymous (demonstrated by the
reviewer with the real binary). A return with changed size or mtime
re-queues through the normal `missing -> changed` path as before.
Change detection is size+mtime at the filesystem's timestamp
resolution (pre-existing limit): a same-second, same-size content
swap is invisible to the walk and restores as unchanged, keeping its
previous face data.

## Apply-time semantics

The indexer and `wdf_faces` read `wfi.conf` once at startup: a saved
edit applies to the NEXT scan or run, not one already in flight. That
is safe without coordinating with the instance lock (#46/#98): the
atomic rename swaps the whole file, so a process starting mid-save
sees either the old config or the new one, never a mix.

## Rejected alternatives

- **Tell the owner to edit wfi.conf in the dashboard file manager:**
  no validation, no atomic write, typo bricks the scan silently.
- **Free-text roots without existence checks:** a typo indexes
  nothing and looks identical to "no faces found".

## Tests

- `settings_test.php`-style unit cases: traversal (`..`), path
  outside `/shares/`, symlink escaping the share, nonexistent
  directory, valid share root, dedupe, count/length caps, empty list.
- Atomic write: saved file content exact, no temp file left behind.
- A `people_post`-style handler test: POST roots through the real
  page (child process) and assert the resulting `wfi.conf`.
- Overlap rejection: ancestor/descendant pairs rejected after
  canonicalization (incl. through a share-level symlink).
- Empty-roots contract test: both binaries succeed on zero roots.
- Root removal: photos under a removed root flip to `missing` on the
  next scan, are excluded from `process`/`cluster` and people counts
  while missing, and recover when the root is re-added; one shared job
  generation per run is pinned (no observed photo is swept); an
  incomplete overlapping walk suppresses the unconfigured sweep.
- `DEVICE-TEST.md` gains the roots-editing step.
