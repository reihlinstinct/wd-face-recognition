<?php
// Scan roots (issue #102): validation rules and the symlink-safe atomic
// save of the roots= line. Run: php apkg/web/tests/roots_test.php
require_once __DIR__ . '/../roots.php';

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

$tmp = tempnam(sys_get_temp_dir(), 'wdf-roots-');
if ($tmp === false || !unlink($tmp) || !mkdir($tmp, 0700)) {
    fwrite(STDERR, "cannot create temp dir\n");
    exit(1);
}
$shares = $tmp . '/shares';
@mkdir($shares . '/Photos/2020', 0777, true);
@mkdir($shares . '/Photos/2021', 0777, true);
@mkdir($shares . '/Media', 0777, true);
$prefix = $shares . '/';

// --- wdf_roots_validate ---
$err = null;
check(wdf_roots_validate([], $err, $prefix) === [], 'empty list valid (shipped default)');
check(wdf_roots_validate(["", "  "], $err, $prefix) === [], 'blank lines collapse to empty');
check(wdf_roots_validate([$prefix . 'Photos'], $err, $prefix) === [$prefix . 'Photos'],
    'share root allowed (scanning wipes nothing)');
check(wdf_roots_validate([$prefix . 'Photos/2020', $prefix . 'Media'], $err, $prefix)
      === [$prefix . 'Photos/2020', $prefix . 'Media'], 'two valid roots accepted');
check(wdf_roots_validate([$prefix . 'Photos/'], $err, $prefix) === [$prefix . 'Photos'],
    'trailing slash normalized');
check(wdf_roots_validate(['/etc'], $err, $prefix) === null, 'outside /shares rejected');
check(wdf_roots_validate(['relative/path'], $err, $prefix) === null, 'relative path rejected');
check(wdf_roots_validate([$prefix . 'Photos/../Media'], $err, $prefix) === null, 'dot-dot rejected');
check(wdf_roots_validate([$prefix . 'Photos,Media'], $err, $prefix) === null, 'comma rejected');
check(wdf_roots_validate([$prefix . 'Photos/nope'], $err, $prefix) === null, 'nonexistent dir rejected');
@symlink('/etc', $shares . '/evil');
check(wdf_roots_validate([$prefix . 'evil'], $err, $prefix) === null, 'share-level link to /etc rejected');
check(wdf_roots_validate([$prefix . 'Photos', $prefix . 'Photos'], $err, $prefix)
      === [$prefix . 'Photos'], 'exact duplicate dropped');
check(wdf_roots_validate([$prefix . 'Photos', $prefix . 'Photos/2020'], $err, $prefix) === null,
    'ancestor/descendant pair rejected (rule 9)');
check(wdf_roots_validate([$prefix . 'Photos/2020', $prefix . 'Photos/2021', $prefix . 'Photos'], $err, $prefix)
      === null, 'descendant listed first still rejected');
$many = [];
for ($i = 0; $i < 17; $i++) {
    @mkdir($shares . '/r' . $i, 0777, true);
    $many[] = $prefix . 'r' . $i;
}
check(wdf_roots_validate($many, $err, $prefix) === null, '17 roots rejected (cap 16)');
check(wdf_roots_validate(array_slice($many, 0, 16), $err, $prefix) !== null, '16 roots accepted');
check(wdf_roots_validate([$prefix . str_repeat('a', 600)], $err, $prefix) === null, 'overlong path rejected');

// --- wdf_roots_save: rewrite only the roots= line ---
$conf = $tmp . '/wfi.conf';
file_put_contents($conf, "# indexer config\ndata_dir=/data\nroots=/old\nmax_depth=8\n");
check(wdf_roots_save($conf, [$prefix . 'Photos', $prefix . 'Media'], $err),
    'save succeeds');
check(file_get_contents($conf)
      === "# indexer config\ndata_dir=/data\nroots={$prefix}Photos,{$prefix}Media\nmax_depth=8\n",
    'only the roots= line changed');
check(wdf_roots_read($conf) === [$prefix . 'Photos', $prefix . 'Media'], 'read-back round-trips');

// missing line is appended, other lines untouched
file_put_contents($conf, "data_dir=/data");
check(wdf_roots_save($conf, [$prefix . 'Photos'], $err), 'append save succeeds');
check(file_get_contents($conf) === "data_dir=/data\nroots={$prefix}Photos\n",
    'missing roots= appended with a newline');

// empty list persists as an empty value
check(wdf_roots_save($conf, [], $err), 'empty save succeeds');
check(file_get_contents($conf) === "data_dir=/data\nroots=\n", 'empty list persists');
check(wdf_roots_read($conf) === [], 'empty roots read back');

// symlink survival (on device wfi.conf links into the persistent path):
// the TARGET is replaced, the link itself must stay a link.
$real = $tmp . '/persistent/wfi.conf';
@mkdir($tmp . '/persistent', 0700, true);
file_put_contents($real, "data_dir=/data\nroots=/old\n");
$link = $tmp . '/app-wfi.conf';
check(symlink($real, $link), 'config symlink created');
check(wdf_roots_save($link, [$prefix . 'Media'], $err), 'save through symlink succeeds');
check(is_link($link) && readlink($link) === $real, 'symlink survived the save');
check(strpos(file_get_contents($real), "roots={$prefix}Media") !== false,
    'target holds the new roots');
check(glob($tmp . '/persistent/*.tmp.*') === [], 'no temp file left behind');

// cleanup
foreach (glob($shares . '/r*') ?: [] as $d) { @rmdir($d); }
@unlink($shares . '/evil');
@rmdir($shares . '/Photos/2020'); @rmdir($shares . '/Photos/2021');
@rmdir($shares . '/Photos'); @rmdir($shares . '/Media'); @rmdir($shares);
@unlink($real); @rmdir($tmp . '/persistent');
@unlink($link); @unlink($conf); @rmdir($tmp);

if ($fails > 0) {
    fwrite(STDERR, "{$fails} check(s) failed\n");
    exit(1);
}
echo "roots_test: all checks passed\n";
