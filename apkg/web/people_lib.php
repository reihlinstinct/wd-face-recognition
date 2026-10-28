<?php
// People pages backend (issue #50, 0.3.0 PR 3d): the stripped OS5 PHP has
// no sqlite extension, so the pages ask wdf_faces for JSON and render it.
// The indexer config (wfi.conf) is key=value lines like the C++ loader's.

// Where the indexer config lives: overridable for tests and packaging,
// normally next to the app.
function wdf_wfi_config_path(string $appDir): string
{
    $env = getenv('WDF_WFI_CONFIG');
    if ($env !== false && $env !== '') {
        return $env;
    }
    return $appDir . '/wfi.conf';
}

// Parses the indexer config (key=value, # comments). Only data_dir
// matters to the pages. Null on an unreadable file.
function wdf_wfi_config_load(string $path): ?array
{
    $text = @file_get_contents($path);
    if ($text === false) {
        return null;
    }
    $out = [];
    foreach (preg_split('/\r?\n/', $text) as $line) {
        $line = trim($line);
        if ($line === '' || $line[0] === '#') {
            continue;
        }
        $eq = strpos($line, '=');
        if ($eq === false) {
            continue;
        }
        $out[trim(substr($line, 0, $eq))] = trim(substr($line, $eq + 1));
    }
    return $out;
}

// Runs wdf_faces with an argv array (no shell, like wdf_run_demo) and
// returns [exit_code, stdout]. stderr lands in the same temp file but is
// not returned: page errors stay generic, details stay in syslog.
function wdf_faces_call(string $appDir, array $args): array
{
    $argv = array_merge([$appDir . '/bin/wdf_faces'], $args);
    $tmp = tempnam(sys_get_temp_dir(), 'wdf-people-');
    if ($tmp === false) {
        return [127, ''];
    }
    $desc = [1 => ['file', $tmp, 'a'], 2 => ['file', $tmp, 'a']];
    $proc = proc_open($argv, $desc, $pipes);
    if (!is_resource($proc)) {
        @unlink($tmp);
        return [127, ''];
    }
    $rc = proc_close($proc);
    $out = (string)file_get_contents($tmp);
    unlink($tmp);
    return [$rc, $out];
}

// Shape checks for the wdf_faces JSON. An empty people list is valid.
function wdf_people_valid($data): bool
{
    if (!is_array($data)) {
        return false;
    }
    foreach ($data as $p) {
        if (!is_array($p)
            || !isset($p['id'], $p['name'], $p['faces'], $p['cover'])
            || !is_int($p['id']) || !is_string($p['name'])
            || !is_int($p['faces']) || !is_string($p['cover'])) {
            return false;
        }
    }
    return true;
}

function wdf_person_valid($data): bool
{
    if (!is_array($data)
        || !isset($data['id'], $data['name'], $data['faces'])
        || !is_int($data['id']) || !is_string($data['name'])
        || !is_array($data['faces'])) {
        return false;
    }
    foreach ($data['faces'] as $f) {
        if (!is_array($f)
            || !isset($f['id'], $f['bbox'], $f['crop'], $f['photo'])
            || !is_int($f['id']) || !is_string($f['bbox'])
            || !is_string($f['crop']) || !is_string($f['photo'])) {
            return false;
        }
    }
    return true;
}

// Validated fetches. Null on any failure (page renders a generic error).
function wdf_people_fetch(string $appDir, string $config): ?array
{
    [$rc, $out] = wdf_faces_call($appDir, ['people', '--config', $config]);
    if ($rc !== 0) {
        return null;
    }
    $data = json_decode($out, true);
    return wdf_people_valid($data) ? $data : null;
}

function wdf_person_fetch(string $appDir, string $config, int $id): ?array
{
    [$rc, $out] = wdf_faces_call($appDir,
        ['person', '--config', $config, '--id', (string)$id]);
    if ($rc !== 0) {
        return null;
    }
    $data = json_decode($out, true);
    return wdf_person_valid($data) ? $data : null;
}

// "Person 7" fallback for unnamed clusters (DESIGN.md).
function wdf_person_label(array $p): string
{
    return $p['name'] !== '' ? $p['name'] : 'Person ' . $p['id'];
}

// A cluster's cover value is "crops/<basename>"; the img endpoint takes
// only the basename (its own allowlist re-checks it).
function wdf_cover_basename(string $cover): string
{
    return basename($cover);
}

// Render helpers, kept pure so the tests can assert escaping.
function wdf_person_card_html(array $p): string
{
    $id = (int)$p['id'];
    $label = htmlspecialchars(wdf_person_label($p), ENT_QUOTES, 'UTF-8');
    $cover = htmlspecialchars(wdf_cover_basename($p['cover']), ENT_QUOTES, 'UTF-8');
    $faces = (int)$p['faces'];
    return '<a class="card" href="person.php?id=' . $id . '">'
        . '<img src="crop.php?f=' . $cover . '" alt="" loading="lazy">'
        . '<span class="name">' . $label . '</span>'
        . '<span class="count">' . $faces . ' faces</span></a>';
}

// The crop grid deliberately does NOT show the source photo path: full
// paths conflict with the planned per-folder visibility rules (review #58).
function wdf_person_face_html(array $f): string
{
    $crop = htmlspecialchars(basename($f['crop']), ENT_QUOTES, 'UTF-8');
    return '<figure><img src="crop.php?f=' . $crop . '" alt="" loading="lazy">'
        . '</figure>';
}

// POST ids arrive as strings; only plain positive ints pass.
function wdf_post_id(string $raw): ?int
{
    if (preg_match('/^\d{1,10}$/', $raw) !== 1) {
        return null;
    }
    $id = (int)$raw;
    return $id > 0 ? $id : null;
}
