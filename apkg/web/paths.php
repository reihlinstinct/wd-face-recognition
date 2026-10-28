<?php
// Photos-folder normalization + validation for the demo runner (issue #24).
// Users paste paths copied from a shell (backslash-escaped spaces) or with a
// trailing slash; folders with spaces and non-ASCII (e.g. Hebrew) names must
// work end to end. exec() always receives the validated path through
// escapeshellarg(), so quote/injection-shaped names are inert.

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
            // resolving outside /shares. Require the REAL location to stay
            // under the real shares root (the demo opens the root with
            // opendir, which follows a root symlink).
            $realRoot = realpath(rtrim($allowedPrefix, '/'));
            $realCand = realpath($cand);
            if ($realRoot === false || $realCand === false) {
                continue;
            }
            if ($realCand !== $realRoot && strpos($realCand, $realRoot . '/') !== 0) {
                continue;
            }
            return $cand;
        }
    }
    return null;
}
