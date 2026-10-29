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

The empty list must be a SUCCESSFUL no-op everywhere, not an error: a
fresh install ships with no roots (privacy first, #56), and erroring
on the shipped default would brick every new install's first run.
Today `load_config` FAILS on an empty list ("config has no roots=")
and the indexer/`wdf_faces` entry points exit nonzero - the
implementation PR changes that. The matrix (CodeRabbit #103): only the
WALK is the no-op; DB work keeps running. `load_config` accepts zero
roots; `wdf_indexer` prints "no roots configured - nothing to scan",
skips walking, exits 0 - but the sweep below still runs; `wdf_faces`
DB operations (`process`, `cluster`, `gc`, people rename/merge) keep
operating on the existing index; read commands are unaffected. The
contract test pins BOTH halves end to end (CodeRabbit #103): on a
fresh install, empty `roots=` -> both binaries succeed, the DB gains
no photos, stderr carries no error; and with a SEEDED index (a photo,
a face, a person present), clearing roots still lets `process`,
`cluster`, `gc`, people rename/merge and all read commands operate on
the existing data - empty roots stops new scanning, it does not freeze
the index.

## Root removal semantics

`sweep_missing` runs per CONFIGURED root, so today a root deleted from
`roots=` is never walked and never swept: its photos stay `scanned`
forever, and their faces keep feeding people identities - stale
clusters and missed missing-file sweeps (CodeRabbit #103). The
implementation PR closes this in the indexer: after sweeping each
configured root, `wdf_indexer` also sweeps every DISTINCT `root`
stored in `photos` that is no longer configured. Such a sweep needs no
walk: nothing under that root was observed in this job, so every one
of its photos is marked `missing`, its faces stop feeding identity
rebuilds, and re-adding the root later re-queues them through the
normal `missing -> changed` path.

One job generation per RUN, shared by every configured root and by the
unconfigured-root sweeps (CodeRabbit #103): `sweep_missing` marks
photos whose `last_seen_job` differs from the sweeping job, so
per-root job ids would mark photos observed earlier in the SAME run as
missing. The zero-root corner follows from the same rule: a run with
no configured roots still opens one job and sweeps every stored root
under it - clearing ALL roots flips every indexed photo to `missing`
(reversible; re-adding a root restores it).

Coverage gate on unconfigured sweeps (CodeRabbit #103): a configured
root's sweep already runs only after a COMPLETE walk (a depth-capped
or unreadable walk is INCOMPLETE and skips the sweep). The
unconfigured-root analog is overlap: a stored root is swept only when
NO configured root overlaps it (neither ancestor nor descendant, on
canonicalized paths - the rule-9 relation). Removing `/shares/Photos`
while `/shares/Photos/2020` stays configured must not sweep the stored
`/shares/Photos` root: files beneath it are still observed under the
surviving subroot, and an ungated sweep would mark observed photos
`missing`. Regression test: index under `/shares/Photos`, narrow the
config to `/shares/Photos/2020`, rescan - photos under `2020` stay
`scanned`, the rest flip to `missing`.

## Missing photos and identity data

A photo in `missing` state must be excluded consistently from
everything identity-shaped: embedding loads, cluster input, people
counts, person details (or its faces detached). The implementation PR
audits each consumer query for the state filter. The root-removal test
verifies the full cycle through `process`/`cluster`, not just the
state column: scan with the root, rescan without it (photos `missing`,
their faces out of clustering and people), re-add the root (photos and
faces back).

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
  generation per run is pinned (no observed photo is swept).
- `DEVICE-TEST.md` gains the roots-editing step.
