<?php
// Configurable output folder (issue #43): validation, migration, and the
// run-cleanup safety contract. Run: php apkg/web/tests/settings_test.php
require_once __DIR__ . '/../settings.php';

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

$tmp = sys_get_temp_dir() . '/wdf-settings-' . getmypid();
$shares = $tmp . '/shares';
$reports = $shares . '/facerecog-reports';
@mkdir($reports . '/demo-out/crops', 0777, true);
@mkdir($shares . '/Photos', 0777, true);
file_put_contents($reports . '/demo-out/index.html', '<html>results</html>');
file_put_contents($reports . '/demo-out/crops/f1.jpg', 'jpeg');
$prefix = $shares . '/';
putenv('WDF_STATE_FILE=' . $tmp . '/state.json');

// --- wdf_normalize_output_dir ---
$err = null;
check(wdf_normalize_output_dir($prefix . 'Photos/face-results', $err, $prefix) === $prefix . 'Photos/face-results', 'valid new subfolder accepted');
check(wdf_normalize_output_dir('', $err, $prefix) === null, 'empty rejected');
check(wdf_normalize_output_dir('/etc/out', $err, $prefix) === null, 'outside prefix rejected');
check(wdf_normalize_output_dir($prefix . 'Photos/../escape', $err, $prefix) === null, 'dot-dot rejected');
check(wdf_normalize_output_dir($prefix . 'Photos', $err, $prefix) === null, 'share ROOT rejected (would wipe the share)');
check(wdf_normalize_output_dir($prefix . 'NoSuchShare/x', $err, $prefix) === null, 'missing share rejected');
check(wdf_normalize_output_dir($prefix . 'Photos/', $err, $prefix) === null, 'trailing-slash share root rejected');
@symlink('/etc', $shares . '/evil');
check(wdf_normalize_output_dir($prefix . 'evil/x', $err, $prefix) === null, 'link to /etc rejected');

// --- default before any setting ---
check(wdf_output_dir($reports) === $reports . '/demo-out', 'default output dir');

// --- reject: target inside the reports share but not the default ---
check(wdf_set_output_dir($prefix . 'facerecog-reports/other', $reports) !== '', 'non-default folder inside reports share rejected');
check(wdf_set_output_dir($prefix . 'facerecog-reports', $reports) !== '', 'reports share root rejected');

// --- reject: non-empty foreign target ---
@mkdir($shares . '/Photos/full', 0777, true);
file_put_contents($shares . '/Photos/full/mine.txt', 'user data');
check(wdf_set_output_dir($prefix . 'Photos/full', $reports) !== '', 'non-empty foreign folder rejected');

// --- happy path: migrate existing results ---
$target = $prefix . 'Photos/face-results';
check(wdf_set_output_dir($target, $reports) === '', 'set + migrate succeeds');
check(wdf_output_dir($reports) === $target, 'setting persisted');
check(is_file($target . '/index.html'), 'results MOVED to the new folder');
check(is_file($target . '/crops/f1.jpg'), 'crops moved');
check(is_file($target . '/' . WDF_OUTPUT_MARKER), 'moved folder got the app marker');
check(!is_dir($reports . '/demo-out'), 'old folder moved away');

// setting the same folder again is a no-op
check(wdf_set_output_dir($target, $reports) === '', 'same folder is a no-op');

// --- run-cleanup safety contract ---
check(wdf_output_dir_replaceable($target), 'marked folder is replaceable');
check(!wdf_output_dir_replaceable($shares . '/Photos/full'), 'foreign non-empty folder is NOT replaceable');
@mkdir($shares . '/Photos/empty', 0777, true);
check(wdf_output_dir_replaceable($shares . '/Photos/empty'), 'empty folder is replaceable');
check(wdf_output_dir_replaceable($shares . '/Photos/gone'), 'missing folder is replaceable');

// --- back to default: old custom folder is now app-marked, so it is a
// legal source for migration; default recreated + marked ---
check(wdf_set_output_dir($prefix . 'facerecog-reports/demo-out', $reports) === '', 'switch back to default');
check(wdf_output_dir($reports) === $reports . '/demo-out', 'default restored');
check(is_file($reports . '/demo-out/index.html'), 'results moved back');

// --- move-tree fallback machinery (cross-device path: CodeRabbit #44) ---
$mvSrc = $tmp . '/mv-src';
@mkdir($mvSrc . '/crops', 0777, true);
file_put_contents($mvSrc . '/index.html', 'x');
file_put_contents($mvSrc . '/crops/f1.jpg', 'yy');
$mvErr = null;
check(wdf_move_tree($mvSrc, $tmp . '/mv-dst', $mvErr), 'move_tree succeeds (rename path)');
check(is_file($tmp . '/mv-dst/crops/f1.jpg') && !is_dir($mvSrc), 'move_tree relocated the tree');
$man = wdf_tree_manifest($tmp . '/mv-dst');
check(array_keys($man) === ['/crops/f1.jpg', '/index.html'], 'manifest lists relative paths');
check($man['/crops/f1.jpg'] === '2:' . sha1('yy') && $man['/index.html'] === '1:' . sha1('x'),
      'manifest carries size+sha1 digests');
// a rewrite keeps the size but changes the digest
file_put_contents($tmp . '/mv-dst/index.html', 'z');
check(wdf_tree_manifest($tmp . '/mv-dst') != $man, 'digest detects same-size rewrite');
file_put_contents($tmp . '/mv-dst/index.html', 'x');
// copy + verify round-trip through the helpers used on the EXDEV path
$cpErr = null;
check(wdf_copy_tree($tmp . '/mv-dst', $tmp . '/mv-dst2', $cpErr), 'copy_tree succeeds');
check(wdf_tree_manifest($tmp . '/mv-dst') == wdf_tree_manifest($tmp . '/mv-dst2'), 'copy verifies identical');
check(wdf_delete_tree($tmp . '/mv-dst2') && !is_dir($tmp . '/mv-dst2'), 'delete_tree removes the copy');

// Copy fallback must not overwrite pre-existing destination files and
// must clean up only its own creations (CodeRabbit PR #44). rename()
// onto a NON-EMPTY dir fails with ENOTEMPTY, which forces the copy path.
$fbSrc = $tmp . '/fb-src';
@mkdir($fbSrc, 0777, true);
// The collision sits at the LAST name (z.txt), so a.txt is really copied
// first - the rollback assertion cannot pass vacuously (CodeRabbit PR #44).
file_put_contents($fbSrc . '/a.txt', 'new');
file_put_contents($fbSrc . '/z.txt', 'fresh');
$fbDst = $tmp . '/fb-dst';
@mkdir($fbDst . '/sub', 0777, true);
file_put_contents($fbDst . '/z.txt', 'ORIGINAL');
file_put_contents($fbDst . '/sub/keep.txt', 'keep');
$fbErr = null;
check(!wdf_move_tree($fbSrc, $fbDst, $fbErr), 'move into non-empty target fails');
check(file_get_contents($fbDst . '/z.txt') === 'ORIGINAL', 'pre-existing destination file untouched');
check(is_file($fbDst . '/sub/keep.txt'), 'unrelated destination content untouched');
check(!is_file($fbDst . '/a.txt'), 'partial copy was rolled back');
check(is_file($fbSrc . '/a.txt') && is_file($fbSrc . '/z.txt'), 'source preserved on failure');

// A symlinked destination file must never be written through (the copy
// would land OUTSIDE the share); dangling links count too.
$symSrc = $tmp . '/sym-src';
@mkdir($symSrc, 0777, true);
file_put_contents($symSrc . '/c.txt', 'payload');
$symDst = $tmp . '/sym-dst';
@mkdir($symDst, 0777, true);
file_put_contents($tmp . '/outside.txt', 'outside');
@symlink($tmp . '/outside.txt', $symDst . '/c.txt');
$symErr = null;
check(!wdf_move_tree($symSrc, $symDst, $symErr), 'symlinked destination rejected');
check(file_get_contents($tmp . '/outside.txt') === 'outside', 'symlink target never written');
check(is_link($symDst . '/c.txt'), 'planted symlink untouched');
check(is_file($symSrc . '/c.txt'), 'source preserved after symlink refusal');

// Old/new overlap is rejected in both directions before anything moves.
check(wdf_set_output_dir($target, $reports) === '', 'move to custom folder again');
check(wdf_set_output_dir($target . '/deeper', $reports) !== '', 'new inside current output rejected');
check(wdf_output_dir($reports) === $target, 'rejected overlap changed nothing');
check(is_file($target . '/index.html'), 'rejected overlap moved nothing');
// (identical path stays a no-op - covered above)

if ($fails > 0) {
    fwrite(STDERR, "{$fails} test(s) failed\n");
    exit(1);
}
echo "settings_test: all assertions passed\n";
