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

if ($fails > 0) {
    fwrite(STDERR, "{$fails} test(s) failed\n");
    exit(1);
}
echo "paths_test: all assertions passed\n";
