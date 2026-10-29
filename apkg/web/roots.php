<?php
// Scan roots (issue #102, design docs/ROOTS-SETTINGS.md): the owner sets
// which shares/folders the indexer scans by editing roots= in wfi.conf;
// this page moves it into the dashboard. One absolute path per line in
// the UI, comma-joined in the file; every line is validated against the
// same rules the demo folder picker uses, plus existence (a scan root
// that does not exist is a typo, not a future state) and the overlap
// ban (nested roots double-scan and confuse the missing sweep).
require_once __DIR__ . '/paths.php';
require_once __DIR__ . '/people_lib.php';

const WDF_ROOTS_MAX = 16;
const WDF_ROOTS_PATH_MAX = 512;

// The configured scan roots, as stored (comma-split, blanks dropped).
// Null only when the config file itself is unreadable.
function wdf_roots_read(string $confPath): ?array
{
    $conf = wdf_wfi_config_load($confPath);
    if ($conf === null) {
        return null;
    }
    $out = [];
    foreach (explode(',', (string)($conf['roots'] ?? '')) as $item) {
        $item = trim($item);
        if ($item !== '') {
            $out[] = $item;
        }
    }
    return $out;
}

// index.php renders these messages unescaped, so embedded user input
// is escaped at build time (ENT_QUOTES, UTF-8).
function wdf_roots_err(string $prefix, string $line): string
{
    return $prefix . htmlspecialchars($line, ENT_QUOTES, 'UTF-8');
}

// Validate the textarea lines. Returns the cleaned root list, or null
// with $err set. An empty list is valid - it is the shipped default
// (privacy first, #56: a fresh install scans nothing).
function wdf_roots_validate(array $lines, ?string &$err,
                            string $allowedPrefix = '/shares/'): ?array
{
    $err = null;
    $roots = [];
    $rootsReal = [];
    $shareRoots = null; // lazy: only computed when a line survives lexing
    foreach ($lines as $raw) {
        $line = rtrim(trim($raw), '/');
        if ($line === '') {
            continue;
        }
        if (strlen($line) > WDF_ROOTS_PATH_MAX) {
            $err = 'Path too long (max ' . WDF_ROOTS_PATH_MAX . ' bytes): '
                . htmlspecialchars(substr($line, 0, 60), ENT_QUOTES, 'UTF-8') . '...';
            return null;
        }
        if (strpos($line, ',') !== false) {
            // roots= is comma-separated: a comma would silently split
            // one root into two.
            $err = wdf_roots_err('Paths must not contain commas: ', $line);
            return null;
        }
        if (strpos($line, $allowedPrefix) !== 0) {
            $err = wdf_roots_err('Each root must be an absolute path under '
                . $allowedPrefix . ': ', $line);
            return null;
        }
        // Deliberately a conservative SUBSTRING match (a folder named
        // "a..b" is rejected too): same guard as settings.php, and a
        // false rejection here costs the owner one renamed folder.
        if (strpos($line, '..') !== false) {
            $err = wdf_roots_err('The path must not contain "..": ', $line);
            return null;
        }
        if (!wdf_path_symlink_components_ok($line, $allowedPrefix)) {
            $err = wdf_roots_err('Symlinks are only allowed at the share level: ', $line);
            return null;
        }
        if (!is_dir($line)) {
            // Unlike the output folder (a future state the app creates),
            // a scan root that does not exist is a typo.
            $err = wdf_roots_err('Folder not found on this NAS: ', $line);
            return null;
        }
        if ($shareRoots === null) {
            $shareRoots = wdf_shares_real_roots($allowedPrefix);
        }
        // Canonical form for the overlap/dedupe checks: resolves the
        // share-level symlink and any ./ or // components.
        $real = wdf_realpath_lenient($line);
        if (!wdf_real_path_under_roots($real, $shareRoots)) {
            $err = wdf_roots_err('The folder must stay inside a share: ', $line);
            return null;
        }
        if (in_array($real, $rootsReal, true)) {
            continue; // exact duplicate after canonicalization: drop
        }
        foreach ($rootsReal as $existing) {
            if (wdf_paths_overlap($real, $existing)) {
                // Nested roots double-scan the subtree per run and their
                // per-root missing sweeps confuse each other (rule 9).
                $err = wdf_roots_err('Roots must not contain one another: ', $line);
                return null;
            }
        }
        $roots[] = $line;
        $rootsReal[] = $real;
        if (count($roots) > WDF_ROOTS_MAX) {
            $err = 'At most ' . WDF_ROOTS_MAX . ' scan roots.';
            return null;
        }
    }
    return $roots;
}

// Persist the roots: rewrite only the roots= line of wfi.conf, every
// other line verbatim; a missing line is appended.
//
// On device the app-dir wfi.conf is a SYMLINK into the persistent data
// path (init.sh), so the write resolves the link and replaces the
// TARGET: temp file in the target's directory, fflush + fsync, then
// rename() over the target. Renaming over the symlink path itself would
// silently replace the link with a regular file and detach the app from
// its persistent config.
function wdf_roots_save(string $confPath, array $roots, ?string &$err): bool
{
    $err = null;
    $text = @file_get_contents($confPath);
    if ($text === false) {
        $err = 'Cannot read the indexer config: ' . $confPath;
        return false;
    }
    $line = 'roots=' . implode(',', $roots);
    $lines = preg_split('/(\r?\n)/', $text, -1, PREG_SPLIT_DELIM_CAPTURE);
    $replaced = false;
    $out = '';
    for ($i = 0; $i < count($lines); $i += 2) {
        $content = $lines[$i];
        $eol = $lines[$i + 1] ?? '';
        if (!$replaced && preg_match('/^\s*roots\s*=/', $content) === 1) {
            $content = $line;
            $replaced = true;
        }
        $out .= $content . $eol;
    }
    if (!$replaced) {
        if ($out !== '' && substr($out, -1) !== "\n") {
            $out .= "\n";
        }
        $out .= $line . "\n";
    }
    $target = realpath($confPath);
    if ($target === false) {
        $target = $confPath; // not a symlink (fresh install): write directly
    }
    $tmpPath = $target . '.tmp.' . getmypid();
    $fh = @fopen($tmpPath, 'w');
    if ($fh === false) {
        $err = 'Cannot write the indexer config - check permissions on '
            . dirname($target);
        return false;
    }
    $ok = fwrite($fh, $out) !== false && fflush($fh) && fsync($fh);
    if (!fclose($fh) || !$ok || !@rename($tmpPath, $target)) {
        @unlink($tmpPath);
        $err = 'Could not save the indexer config - nothing was changed.';
        return false;
    }
    return true;
}
