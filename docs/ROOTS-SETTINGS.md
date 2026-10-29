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
8. The empty list is valid - it is the shipped default (the indexer
   scans nothing).

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
- `DEVICE-TEST.md` gains the roots-editing step.
