<?php
// Tests for wdf_normalize_photos_dir (issue #24). Run: php apkg/web/tests/paths_test.php
// No framework: plain asserts over a temp tree with spaces, backslashes and
// Hebrew folder names.
require_once __DIR__ . '/../paths.php';

$tmp = sys_get_temp_dir() . '/wdf-paths-' . getmypid();
@mkdir($tmp . '/shares/rei/iPhone Camera Roll Backup', 0777, true);
@mkdir($tmp . '/shares/תמונות/משפחה', 0777, true);
@mkdir($tmp . '/shares/back\slash', 0777, true);
$prefix = $tmp . '/shares/';

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

$withSpaces = $prefix . 'rei/iPhone Camera Roll Backup';

// Raw path with spaces resolves.
check(wdf_normalize_photos_dir($withSpaces, $prefix) === $withSpaces, 'spaces raw');
// Shell-style backslash escapes resolve to the real folder.
check(wdf_normalize_photos_dir($prefix . 'rei/iPhone\\ Camera\\ Roll\\ Backup', $prefix) === $withSpaces, 'shell-escaped spaces');
// Trailing slash is dropped.
check(wdf_normalize_photos_dir($withSpaces . '/', $prefix) === $withSpaces, 'trailing slash');
// The exact user input that failed tonight: escapes AND trailing slash.
check(wdf_normalize_photos_dir($prefix . 'rei/iPhone\\ Camera\\ Roll\\ Backup/', $prefix) === $withSpaces, 'escaped + trailing slash');
// Hebrew folder names resolve.
check(wdf_normalize_photos_dir($prefix . 'תמונות/משפחה', $prefix) === $prefix . 'תמונות/משפחה', 'Hebrew names');
// A genuine backslash in a real folder name wins over unescaping.
check(wdf_normalize_photos_dir($prefix . 'back\slash', $prefix) === $prefix . 'back\slash', 'real backslash dir');
// Symlink containment: escaping the prefix is rejected, staying inside is fine.
@symlink('/etc', $tmp . '/shares/escape');
check(wdf_normalize_photos_dir($prefix . 'escape', $prefix) === null, 'symlink escaping prefix rejected');
@symlink($tmp . '/shares/rei', $tmp . '/shares/alias-in');
check(wdf_normalize_photos_dir($prefix . 'alias-in', $prefix) === $prefix . 'alias-in', 'symlink within prefix allowed');
// OS5 share layout (issue #28): a share is a symlink at the shares root
// into the data volume; the same-name rule accepts it, and nested paths
// inside it resolve.
@mkdir($tmp . '/vol/משפחה/חופשה', 0777, true);
@symlink($tmp . '/vol/משפחה', $tmp . '/shares/משפחה');
check(wdf_normalize_photos_dir($prefix . 'משפחה', $prefix) === $prefix . 'משפחה', 'share symlink accepted');
check(wdf_normalize_photos_dir($prefix . 'משפחה/חופשה', $prefix) === $prefix . 'משפחה/חופשה', 'nested path inside share symlink accepted');
// Nested symlinks are never followed, even into an allowed share
// (consistent with the browser listing and the lstat-based scanner).
@symlink($tmp . '/vol/משפחה', $tmp . '/shares/rei/shortcut');
check(wdf_normalize_photos_dir($prefix . 'rei/shortcut', $prefix) === null, 'nested link into share rejected');
@symlink($tmp . '/vol/משפחה/חופשה', $tmp . '/vol/משפחה/inlink');
check(wdf_normalize_photos_dir($prefix . 'משפחה/inlink', $prefix) === null, 'link inside a symlinked share rejected');
// A root-level link into the OS filesystem under another name is not a share.
@symlink('/etc', $tmp . '/shares/evil');
check(wdf_normalize_photos_dir($prefix . 'evil', $prefix) === null, 'root link to /etc rejected');
// A dangling share link is rejected.
@symlink($tmp . '/vol/gone', $tmp . '/shares/Gone');
check(wdf_normalize_photos_dir($prefix . 'Gone', $prefix) === null, 'dangling share link rejected');
// Guards: outside prefix, "..", missing folder, empty input.
check(wdf_normalize_photos_dir('/etc', $prefix) === null, 'outside prefix rejected');
check(wdf_normalize_photos_dir($prefix . '..', $prefix) === null, 'dotdot rejected');
check(wdf_normalize_photos_dir($prefix . 'missing-folder', $prefix) === null, 'missing folder rejected');
check(wdf_normalize_photos_dir('', $prefix) === null, 'empty rejected');
// Injection-shaped input is not a dir, so it never reaches exec.
check(wdf_normalize_photos_dir($prefix . "x'; touch /tmp/wdf-pwned; echo '", $prefix) === null, 'injection string rejected');
check(!file_exists('/tmp/wdf-pwned'), 'no injection side effect');
// escapeshellarg quoting of a quote-bearing name stays shell-safe.
$quoted = escapeshellarg($prefix . "it's a dir");
check(strpos($quoted, "'") === 0 && substr_count($quoted, "'\\''") === 1, 'escapeshellarg quotes safely');

// wdf_path_is_within: the demo-out source guard (CodeRabbit PR #34).
check(wdf_path_is_within('/shares/facerecog-reports/demo-out', '/shares/facerecog-reports/demo-out'),
      'demo-out itself is within demo-out');
check(wdf_path_is_within('/shares/facerecog-reports/demo-out/crops', '/shares/facerecog-reports/demo-out'),
      'a demo-out subfolder is within demo-out');
check(!wdf_path_is_within('/shares/facerecog-reports/demo-outside', '/shares/facerecog-reports/demo-out'),
      'a sibling with a shared name prefix is NOT within demo-out');
check(!wdf_path_is_within('/shares/facerecog-reports', '/shares/facerecog-reports/demo-out'),
      'the parent share is not within demo-out');
check(!wdf_path_is_within('/shares/Public/photos', '/shares/facerecog-reports/demo-out'),
      'an unrelated folder is not within demo-out');

// wdf_realpath_lenient: ./ and not-yet-existing tails resolve so the
// demo-out source guard cannot be bypassed with a non-canonical path
// (CodeRabbit PR #34).
$rpTmp = sys_get_temp_dir() . '/wdf-rp-' . getmypid();
@mkdir($rpTmp . '/demo-out/crops', 0777, true);
check(wdf_realpath_lenient($rpTmp . '/demo-out') === realpath($rpTmp . '/demo-out'),
      'existing path resolves to its realpath');
check(wdf_realpath_lenient($rpTmp . '/missing/leaf') === realpath($rpTmp) . '/missing/leaf',
      'missing tail is appended to the resolved ancestor');
check(wdf_path_is_within(wdf_realpath_lenient($rpTmp . '/./demo-out/crops'),
                         wdf_realpath_lenient($rpTmp . '/demo-out')),
      'a ./ variant of a demo-out subfolder is caught by the resolved guard');
check(!wdf_path_is_within(wdf_realpath_lenient($rpTmp . '/./other'),
                          wdf_realpath_lenient($rpTmp . '/demo-out')),
      'an unrelated ./ path still passes the resolved guard');

if ($fails > 0) {
    fwrite(STDERR, "{$fails} test(s) failed\n");
    exit(1);
}
echo "paths_test: all assertions passed\n";
