<?php
// Photos-folder normalization + validation for the demo runner (issue #24).
// Users paste paths copied from a shell (backslash-escaped spaces) or with a
// trailing slash; folders with spaces and non-ASCII (e.g. Hebrew) names must
// work end to end. exec() always receives the validated path through
// escapeshellarg(), so quote/injection-shaped names are inert.

// On WD OS5 every real share under /shares is a SYMLINK into the data
// volume (e.g. /shares/Photos -> /mnt/HD/HD_a2/Photos; WD KB a_id/19203
// documents /shares/ as "the symbolic links to the shares"). A guard that
// only accepts paths resolving under realpath('/shares') therefore hides
// every real share (issue #28). The allowed real roots are the real shares
// root plus the resolved target of each direct child symlink of the shares
// root that looks like a share: it crosses onto another filesystem (the
// data volume) or keeps the share name as its basename. A root-level link
// into the OS filesystem under a different name (e.g. -> /etc) never
// becomes a root; /shares is root-managed, so its direct children are the
// trust boundary.
function wdf_shares_real_roots(string $allowedPrefix = '/shares/'): array
{
    $realRoot = realpath(rtrim($allowedPrefix, '/'));
    if ($realRoot === false) {
        return [];
    }
    $roots = [$realRoot];
    $rootStat = @stat($realRoot);
    $names = @scandir($realRoot);
    if ($names === false) {
        return $roots;
    }
    foreach ($names as $name) {
        if ($name === '.' || $name === '..' || $name[0] === '.') {
            continue;
        }
        $full = $realRoot . '/' . $name;
        if (!is_link($full)) {
            continue; // plain dirs are already under the real root
        }
        $target = realpath($full);
        if ($target === false || !is_dir($target)) {
            continue; // dangling or non-directory link
        }
        $targetStat = @stat($target);
        $crossesMount = $rootStat !== false && $targetStat !== false
            && $targetStat['dev'] !== $rootStat['dev'];
        if ($crossesMount || basename($name) === basename($target)) {
            $roots[] = $target;
        }
    }
    return $roots;
}

// At most one symlink component is allowed in a candidate path, and only as
// the DIRECT CHILD of the shares root (the OS5 share link). Nested symlinks
// are never followed - same rule as the browser listing and the scanner's
// lstat-based walk, so a pasted path cannot enter through a link the
// browser would hide (issue #28 review).
function wdf_path_symlink_components_ok(string $cand, string $allowedPrefix = '/shares/'): bool
{
    $rootPath = rtrim($allowedPrefix, '/');
    $rel = trim(substr($cand, strlen($rootPath)), '/');
    if ($rel === '') {
        return true; // the shares root itself
    }
    $cur = $rootPath;
    $i = 0;
    foreach (explode('/', $rel) as $comp) {
        $cur .= '/' . $comp;
        if (is_link($cur) && $i > 0) {
            return false; // nested symlink: never followed
        }
        $i++;
    }
    return true;
}

// True when an already-resolved real path equals or lives under one of the
// allowed roots.
function wdf_real_path_under_roots(string $realPath, array $roots): bool
{
    foreach ($roots as $root) {
        if ($realPath === $root || strpos($realPath, $root . '/') === 0) {
            return true;
        }
    }
    return false;
}

// Returns the validated absolute path of an existing directory, or null.
// Guards (allowed prefix, no "..") are applied to the FINAL candidate path,
// never to the raw input. A genuine backslash in a real folder name still
// wins, because the raw input is tried before the unescaped form.
function wdf_normalize_photos_dir(string $input, string $allowedPrefix = '/shares/'): ?string
{
    $input = trim($input);
    if ($input === '') {
        return null;
    }

    // Trailing slashes are pure normalization (a dir name never ends in
    // "/"), so strip them from every candidate up front. Shell-style
    // unescape ("\x" -> "x") is only a fallback: the raw form is tried
    // first, so a genuine backslash in a real folder name still wins.
    $base = rtrim($input, '/');
    if ($base === '') {
        $base = '/';
    }
    $candidates = [$base];
    $unescaped = rtrim(preg_replace('/\\\\(.)/s', '$1', $base), '/');
    if ($unescaped === '') {
        $unescaped = '/';
    }
    if ($unescaped !== $base) {
        $candidates[] = $unescaped;
    }

    foreach ($candidates as $cand) {
        if (strpos($cand, $allowedPrefix) !== 0) {
            continue;
        }
        if (strpos($cand, '..') !== false) {
            continue;
        }
        if (is_dir($cand)) {
            // is_dir() follows symlinks: a candidate such as
            // /shares/link -> /etc would pass the lexical checks while
            // resolving outside the allowed roots. Require the REAL
            // location to stay under an allowed root: the real shares
            // root or a share's real symlink target (OS5 layout - the
            // demo opens the root with opendir, which follows a root
            // symlink, and share roots resolve onto the data volume).
            if (!wdf_path_symlink_components_ok($cand, $allowedPrefix)) {
                continue;
            }
            $realCand = realpath($cand);
            if ($realCand === false) {
                continue;
            }
            if (!wdf_real_path_under_roots($realCand, wdf_shares_real_roots($allowedPrefix))) {
                continue;
            }
            return $cand;
        }
    }
    return null;
}
