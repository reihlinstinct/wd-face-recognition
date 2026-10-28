<?php
// Folder browser for the demo photo-folder picker (issue #26).
// Server-rendered, no JavaScript: plain GET links descend into /shares and a
// "use this folder" link prefills the demo form. Listing is restricted to
// /shares (reusing wdf_normalize_photos_dir plus an allowed-real-roots
// guard). OS5 share symlinks at the shares root are followed one hop (that
// is where the shares actually live, issue #28); symlinks inside shares are
// never followed. Every name/path is HTML-escaped on render.
require_once __DIR__ . '/paths.php';

const WDF_BROWSE_MAX_PHOTOS = 1000; // same caps as the C++ demo scanner
const WDF_BROWSE_MAX_DEPTH = 8;

// Recursive JPEG count using the demo scanner's caps, so the number shown
// matches what a run would actually find. Symlinks are never followed.
// Returns the count (capped at $maxPhotos), or -1 when $dir is unreadable.
function wdf_browse_count_jpegs(string $dir, int $maxPhotos = WDF_BROWSE_MAX_PHOTOS, int $maxDepth = WDF_BROWSE_MAX_DEPTH): int
{
    $count = 0;
    $stack = [[$dir, 0]];
    while ($stack !== [] && $count < $maxPhotos) {
        [$cur, $depth] = array_pop($stack);
        $handle = @opendir($cur);
        if ($handle === false) {
            if ($cur === $dir) {
                return -1;
            }
            continue; // unreadable nested folder: skip, like the C++ scanner
        }
        while (($entry = readdir($handle)) !== false && $count < $maxPhotos) {
            if ($entry === '.' || $entry === '..') {
                continue;
            }
            $full = $cur . '/' . $entry;
            if (is_link($full)) {
                continue; // never follow symlinks
            }
            if (is_dir($full)) {
                if ($depth + 1 < $maxDepth) {
                    $stack[] = [$full, $depth + 1];
                }
            } elseif (is_file($full)
                && strlen($entry) > 4
                && preg_match('/\.jpe?g$/i', $entry) === 1) {
                $count++;
            }
        }
        closedir($handle);
    }
    return $count;
}

// Validates $dir under $allowedPrefix and lists its immediate subdirectories.
// Returns ['dir' => normalized path, 'jpeg_count' => n, 'subdirs' => [...]]
// or null when $dir is outside the prefix, missing, or a symlink escape.
function wdf_browse_list(string $dir, string $allowedPrefix = '/shares/'): ?array
{
    $rootPath = rtrim($allowedPrefix, '/');
    $norm = wdf_normalize_photos_dir($dir, $allowedPrefix);
    if ($norm === null) {
        // The prefix root itself ("/shares") does not match the "/shares/"
        // prefix check; accept it explicitly.
        $cand = rtrim(trim($dir), '/');
        if ($cand === $rootPath && is_dir($cand)) {
            $norm = $cand;
        }
    }
    if ($norm === null) {
        return null;
    }
    // Symlink guard on the selected path: its real location must stay under
    // an allowed root (the real shares root or a share's real target - on
    // OS5 each share under /shares is a symlink into the data volume).
    if (!wdf_path_symlink_components_ok($norm, $allowedPrefix)) {
        return null; // nested symlinks are never followed
    }
    $roots = wdf_shares_real_roots($allowedPrefix);
    $realDir = realpath($norm);
    if ($roots === [] || $realDir === false
        || !wdf_real_path_under_roots($realDir, $roots)) {
        return null;
    }
    $atSharesRoot = ($realDir === $roots[0]);
    $names = scandir($realDir);
    if ($names === false) {
        return null;
    }
    $subdirs = [];
    foreach ($names as $name) {
        if ($name === '.' || $name === '..' || $name[0] === '.') {
            continue; // no dotfiles (.AppleDouble and friends), no ".." escape
        }
        $full = $realDir . '/' . $name;
        if (is_link($full)) {
            // Share-level symlinks live at the shares root (OS5 layout);
            // nested symlinks are never followed, so loops cannot form.
            if (!$atSharesRoot) {
                continue;
            }
            $target = realpath($full);
            if ($target === false || !is_dir($target)
                || !wdf_real_path_under_roots($target, $roots)) {
                continue;
            }
            $countDir = $target;
        } elseif (!is_dir($full)) {
            continue;
        } else {
            $countDir = $full;
        }
        $subdirs[] = [
            'name' => $name,
            'path' => rtrim($norm, '/') . '/' . $name,
            'jpeg_count' => wdf_browse_count_jpegs($countDir),
        ];
    }
    usort($subdirs, function ($a, $b) {
        return strnatcasecmp($a['name'], $b['name']);
    });
    return [
        'dir' => rtrim($norm, '/') === '' ? '/' : rtrim($norm, '/'),
        'jpeg_count' => wdf_browse_count_jpegs($realDir),
        'subdirs' => $subdirs,
    ];
}

// Renders the browser as an HTML fragment. All names and paths are escaped
// (htmlspecialchars for display, urlencode for query params).
function wdf_browse_render(array $listing, string $allowedPrefix = '/shares/'): string
{
    $rootPath = rtrim($allowedPrefix, '/');
    $dir = $listing['dir'];
    $h = function ($s) {
        return htmlspecialchars((string)$s, ENT_QUOTES, 'UTF-8');
    };
    $u = function ($s) {
        return urlencode((string)$s);
    };
    $fmt = function ($n) {
        if ($n < 0) {
            return 'unreadable';
        }
        return $n >= WDF_BROWSE_MAX_PHOTOS ? WDF_BROWSE_MAX_PHOTOS . '+' : (string)$n;
    };
    $html = '<div class="browse">'
        . '<p>Browsing <b>' . $h($dir) . '</b> - ' . $fmt($listing['jpeg_count']) . ' JPEGs in this folder tree.</p>'
        . '<p><a href="?select=' . $u($dir) . '">Use this folder</a></p>'
        . '<ul>';
    $parent = dirname($dir);
    if ($parent === $rootPath || strpos($parent, $allowedPrefix) === 0) {
        $html .= '<li><a href="?browse=' . $u($parent) . '">.. (up)</a></li>';
    }
    foreach ($listing['subdirs'] as $sub) {
        $html .= '<li><a href="?browse=' . $u($sub['path']) . '">'
            . $h($sub['name']) . '/</a> - ' . $fmt($sub['jpeg_count']) . ' JPEGs</li>';
    }
    if ($listing['subdirs'] === []) {
        $html .= '<li>(no subfolders)</li>';
    }
    return $html . '</ul></div>';
}
