<?php
// Configurable output folder (issue #43): the user picks where the demo
// results (grouped index.html + face crops) live - a subfolder inside an
// existing share. Persisted in state.json next to the PIN in the reports
// share. Safety contract:
//  - never a share ROOT: each run replaces the folder's contents, so
//    pointing at a share root would wipe the share;
//  - never over the app's own files (PIN / lock / state in the reports
//    share), except the default folder itself;
//  - a run refuses to wipe a non-empty folder that lacks the app's
//    marker file - it was not created by this app;
//  - when the setting changes, existing results are MOVED (rename) to
//    the new folder, so nothing the user already grouped is lost.
require_once __DIR__ . '/paths.php';

const WDF_OUTPUT_MARKER = '.wdf-demo-output';

function wdf_default_output(string $reportsDir = '/shares/facerecog-reports'): string
{
    return $reportsDir . '/demo-out';
}

function wdf_state_file(string $reportsDir = '/shares/facerecog-reports'): string
{
    $env = getenv('WDF_STATE_FILE');
    return $env !== false ? (string)$env : $reportsDir . '/state.json';
}

// The configured output folder (validated when set; the stored value is
// trusted as-is so the results endpoint stays a pure read).
function wdf_output_dir(string $reportsDir = '/shares/facerecog-reports'): string
{
    $f = wdf_state_file($reportsDir);
    if (is_file($f)) {
        $state = json_decode((string)file_get_contents($f), true);
        if (is_array($state) && !empty($state['output_dir'])
            && is_string($state['output_dir'])) {
            return $state['output_dir'];
        }
    }
    return wdf_default_output($reportsDir);
}

// Validate a user-chosen output folder; the tail may not exist yet, the
// share must. Returns the normalized path, or null with $err set.
function wdf_normalize_output_dir(string $input, ?string &$err,
                                  string $allowedPrefix = '/shares/'): ?string
{
    $err = null;
    $input = rtrim(trim($input), '/');
    if ($input === '') {
        $err = 'Empty folder path.';
        return null;
    }
    if (strpos($input, $allowedPrefix) !== 0) {
        $err = 'The folder must be under ' . $allowedPrefix . '.';
        return null;
    }
    if (strpos($input, '..') !== false) {
        $err = 'The path must not contain "..".';
        return null;
    }
    $rel = substr($input, strlen($allowedPrefix));
    $parts = explode('/', $rel);
    if (count($parts) < 2 || $parts[0] === '' || $parts[1] === '') {
        // Runs replace the folder's contents: a share root would wipe
        // the whole share.
        $err = 'Pick a subfolder inside a share, not the share itself'
            . ' (each run replaces the folder contents).';
        return null;
    }
    if (!is_dir($allowedPrefix . $parts[0])) {
        $err = 'Share not found: ' . $parts[0];
        return null;
    }
    if (!wdf_path_symlink_components_ok($input, $allowedPrefix)) {
        $err = 'Symlinks are only allowed at the share level.';
        return null;
    }
    $real = wdf_realpath_lenient($input);
    if (!wdf_real_path_under_roots($real, wdf_shares_real_roots($allowedPrefix))) {
        $err = 'The folder must stay inside a share.';
        return null;
    }
    return $input;
}

// May a run replace this folder's contents? Only when missing, empty, or
// clearly created by this app (marker file present).
function wdf_output_dir_replaceable(string $dir): bool
{
    if (!is_dir($dir)) {
        return true;
    }
    $names = @scandir($dir);
    if ($names === false) {
        return false;
    }
    $names = array_diff($names, ['.', '..']);
    if ($names === []) {
        return true;
    }
    return in_array(WDF_OUTPUT_MARKER, $names, true);
}

function wdf_mark_output_dir(string $dir): void
{
    @file_put_contents($dir . '/' . WDF_OUTPUT_MARKER, "wd-face-recognition demo output\n");
}

// Recursive copy used when rename() cannot cross filesystems (EXDEV -
// e.g. a share living on a USB drive). Symlinks are copied as files'
// targets are followed by copy(); the demo output never contains any.
function wdf_copy_tree(string $src, string $dst, ?string &$err, array &$created = []): bool
{
    $names = @scandir($src);
    if ($names === false) {
        $err = 'cannot list ' . $src;
        return false;
    }
    // A symlinked destination would make copy()/mkdir write THROUGH the
    // link, outside the selected share (CodeRabbit PR #44). is_link uses
    // lstat, so dangling links are caught too (file_exists misses them).
    if (is_link($dst)) {
        $err = 'destination is a symlink: ' . $dst;
        return false;
    }
    if (!is_dir($dst)) {
        if (!@mkdir($dst, 0777, true)) {
            $err = 'cannot create ' . $dst;
            return false;
        }
        $created[] = $dst;
    }
    foreach ($names as $n) {
        if ($n === '.' || $n === '..') {
            continue;
        }
        $s = $src . '/' . $n;
        $d = $dst . '/' . $n;
        if (is_dir($s) && !is_link($s)) {
            if (!wdf_copy_tree($s, $d, $err, $created)) {
                return false;
            }
        } else {
            // Never overwrite a pre-existing destination file; on abort only
            // THIS attempt's creations are removed (CodeRabbit PR #44).
            if (is_link($d) || file_exists($d)) {
                $err = 'target already exists: ' . $d;
                return false;
            }
            if (!@copy($s, $d)) {
                // copy() can create the destination and still return false;
                // remove it only because it provably did not exist above.
                if (is_file($d) && !is_link($d)) {
                    @unlink($d);
                }
                $err = 'cannot copy ' . $n;
                return false;
            }
            $created[] = $d;
        }
    }
    return true;
}

// Undo a failed copy attempt: remove only what the attempt itself
// created, deepest first, never pre-existing destination content.
function wdf_remove_created(array $created): void
{
    for ($i = count($created) - 1; $i >= 0; $i--) {
        if (is_dir($created[$i])) {
            @rmdir($created[$i]);
        } else {
            @unlink($created[$i]);
        }
    }
}

// Relative path => "size:sha1" inventory, for post-copy verification
// (content digests, CodeRabbit PR #44 - size alone cannot tell a rewrite
// from a copy).
function wdf_tree_manifest(string $root, string $rel = ''): array
{
    $out = [];
    $names = @scandir($root . $rel) ?: [];
    foreach ($names as $n) {
        if ($n === '.' || $n === '..') {
            continue;
        }
        $p = $rel . '/' . $n;
        if (is_dir($root . $p) && !is_link($root . $p)) {
            $out += wdf_tree_manifest($root, $p);
        } else {
            $out[$p] = @filesize($root . $p) . ':' . @sha1_file($root . $p);
        }
    }
    return $out;
}

function wdf_delete_tree(string $root): bool
{
    $names = @scandir($root);
    if ($names === false) {
        return false;
    }
    $ok = true;
    foreach ($names as $n) {
        if ($n === '.' || $n === '..') {
            continue;
        }
        $p = $root . '/' . $n;
        if (is_dir($p) && !is_link($p)) {
            $ok = wdf_delete_tree($p) && $ok;
        } else {
            $ok = @unlink($p) && $ok;
        }
    }
    return @rmdir($root) && $ok;
}

// Move $old to $new (CodeRabbit PR #44): rename() only works within one
// filesystem, so on failure fall back to copy + verify + delete. The
// ORIGINAL is preserved unless the copy verified complete; a partial
// copy at the target is removed again.
function wdf_move_tree(string $old, string $new, ?string &$err): bool
{
    if (@rename($old, $new)) {
        return true;
    }
    $created = [];
    if (!wdf_copy_tree($old, $new, $err, $created)) {
        wdf_remove_created($created);
        $err = 'Could not move the existing results to the new folder: ' . $err;
        return false;
    }
    $srcManifest = wdf_tree_manifest($old);
    if ($srcManifest != wdf_tree_manifest($new)) {
        wdf_remove_created($created);
        $err = 'Copy verification failed - the original results were left in place.';
        return false;
    }
    // TOCTOU (CodeRabbit PR #44): if the source changed WHILE copying
    // (e.g. a run still writing), the copy is stale - drop it, keep the
    // source, move nothing.
    if ($srcManifest != wdf_tree_manifest($old)) {
        wdf_remove_created($created);
        $err = 'The source folder changed during the copy - nothing was moved.';
        return false;
    }
    if (!wdf_delete_tree($old)) {
        $err = 'The results were copied but the old folder could not be removed'
            . ' - remove it manually.';
        return false;
    }
    return true;
}

// Change the output folder: validate, reject a non-empty target, MOVE the
// existing results over (rename - same data volume on OS5), mark the
// folder as app-owned, persist atomically. Returns '' on success, else
// an error message.
function wdf_set_output_dir(string $input,
                            string $reportsDir = '/shares/facerecog-reports'): string
{
    $err = null;
    $new = wdf_normalize_output_dir($input, $err, dirname($reportsDir) . '/');
    if ($new === null) {
        return $err;
    }
    $old = wdf_output_dir($reportsDir);
    if ($new === $old) {
        return '';
    }
    // Inside the reports share only the default folder may hold results:
    // the share root itself holds the PIN, the lock and state.json.
    if ($new !== wdf_default_output($reportsDir)
        && wdf_path_is_within(wdf_realpath_lenient($new), wdf_realpath_lenient($reportsDir))) {
        return 'Inside the facerecog-reports share only the default folder (demo-out)'
            . ' may hold results - the share holds the app PIN and state.';
    }
    // Old/new must not overlap in either direction (CodeRabbit PR #44):
    // moving a folder into itself or into its own child breaks the tree.
    // Identical paths returned above stay the no-op.
    if (wdf_paths_overlap(wdf_realpath_lenient($new), wdf_realpath_lenient($old))) {
        return 'The new folder overlaps the current results folder'
            . ' - choose a different location.';
    }
    if (is_dir($new) && !wdf_output_dir_replaceable($new)) {
        return 'The chosen folder is not empty - pick an empty or new folder'
            . ' (each run replaces its contents).';
    }
    $parent = dirname($new);
    if (!is_dir($parent) && !@mkdir($parent, 0777, true)) {
        return 'Could not create ' . $parent . ' - check share permissions.';
    }
    if (is_dir($old)) {
        $moveErr = null;
        if (!wdf_move_tree($old, $new, $moveErr)) {
            return $moveErr;
        }
    } elseif (!is_dir($new) && !@mkdir($new, 0777, true)) {
        return 'Could not create the new folder - check share permissions.';
    }
    // Marker also for legacy (pre-0.2.5) content moved from the default.
    wdf_mark_output_dir($new);
    $f = wdf_state_file($reportsDir);
    $tmp = $f . '.tmp';
    if (@file_put_contents($tmp, json_encode(['output_dir' => $new], JSON_PRETTY_PRINT)) === false
        || !@rename($tmp, $f)) {
        return 'The results were moved but the setting could not be saved'
            . ' - check that the facerecog-reports share is writable.';
    }
    return '';
}
