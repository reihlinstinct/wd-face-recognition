<?php
// Handler-level scan-roots test (issue #102): POSTing set_roots through
// the REAL index.php rewrites only the roots= line of wfi.conf; an
// invalid root and a wrong PIN leave the file untouched. The page runs
// in a CHILD process (its success path exits); assertions run here.
// Run: php apkg/web/tests/roots_post_test.php

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

$tmp = tempnam(sys_get_temp_dir(), 'wdf-rp-');
if ($tmp === false || !unlink($tmp) || !mkdir($tmp, 0700)) {
    fwrite(STDERR, "cannot create temp dir\n");
    exit(1);
}
$shares = $tmp . '/shares';
@mkdir($shares . '/Photos', 0777, true);
@mkdir($shares . '/Media', 0777, true);
@mkdir($tmp . '/sess', 0700, true);

file_put_contents($tmp . '/demo-pin.txt', "0123abcd4567\n");
$conf = $tmp . '/wfi.conf';
file_put_contents($conf, "data_dir={$tmp}/data\nroots=\n");
putenv('WDF_PIN_FILE=' . $tmp . '/demo-pin.txt');
putenv('WDF_WFI_CONFIG=' . $conf);
putenv('WDF_STATE_FILE=' . $tmp . '/state.json');
putenv('WDF_SHARES_PREFIX=' . $shares . '/');

$desc = [1 => ['file', $tmp . '/child.out', 'w'],
         2 => ['file', $tmp . '/child.err', 'w']];
$proc = proc_open(
    [PHP_BINARY, __DIR__ . '/roots_post_child.php', $tmp,
     $shares . '/Photos', $shares . '/Media', $shares . '/Nope'],
    $desc, $pipes);
check(is_resource($proc), 'child process launches');

$wait = function (string $f) use ($tmp): bool {
    $deadline = microtime(true) + 10;
    while (microtime(true) < $deadline) {
        if (is_file($tmp . '/' . $f)) {
            return true;
        }
        usleep(50000);
    }
    return false;
};
$gate = function (string $f) use ($tmp): void {
    file_put_contents($tmp . '/' . $f, '1');
};

check($wait('ready'), 'child rendered the page (GET)');

$gate('go-badpin');
check($wait('go-badpin-done'), 'wrong-PIN POST ran');
check(file_get_contents($conf) === "data_dir={$tmp}/data\nroots=\n",
    'wrong PIN leaves the config untouched');

$gate('go-invalid');
check($wait('go-invalid-done'), 'invalid POST ran');
check(file_get_contents($conf) === "data_dir={$tmp}/data\nroots=\n",
    'invalid root leaves the config untouched');

$gate('go-valid');
check($wait('go-valid-done'), 'valid save POST ran');
check(file_get_contents($conf)
      === "data_dir={$tmp}/data\nroots={$shares}/Photos,{$shares}/Media\n",
    'valid save rewrites only the roots= line');

$rc = is_resource($proc) ? proc_close($proc) : -1;
check($rc === 0, 'child exits 0');

// Cleanup (best effort).
@rmdir($shares . '/Photos'); @rmdir($shares . '/Media'); @rmdir($shares);
foreach (glob($tmp . '/sess/*') ?: [] as $f) { @unlink($f); }
foreach (glob($tmp . '/go-*') ?: [] as $f) { @unlink($f); }
foreach (['demo-pin.txt', 'wfi.conf', 'ready', 'child.out', 'child.err'] as $f) {
    @unlink($tmp . '/' . $f);
}
@rmdir($tmp . '/sess'); @rmdir($tmp);

if ($fails > 0) {
    fwrite(STDERR, "{$fails} check(s) failed\n");
    exit(1);
}
echo "roots_post_test: all checks passed\n";
