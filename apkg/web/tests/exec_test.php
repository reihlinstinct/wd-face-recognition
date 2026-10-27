<?php
// Tests for wdf_run_demo (Hebrew/space argv survive under a C locale).
// Run: php apkg/web/tests/exec_test.php
require_once __DIR__ . '/../run.php';

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

// Worst case: the web SAPI runs under a non-UTF-8 locale.
setlocale(LC_CTYPE, 'C');

$marker = sys_get_temp_dir() . '/wdf-argv-' . getmypid() . '.bin';
$hebrew = '/shares/תמונות/משפחה';
[$rc, $lines] = wdf_run_demo([
    PHP_BINARY, '-r',
    'file_put_contents($argv[1], $argv[2]); echo "rc-line\n"; fwrite(STDERR, "err-line\n");',
    $marker, $hebrew,
]);
check($rc === 0, 'exit code');
check(file_get_contents($marker) === $hebrew, 'Hebrew argv bytes intact under LC_CTYPE=C');

$spaced = '/shares/rei/iPhone Camera Roll Backup';
[$rc2, ] = wdf_run_demo([PHP_BINARY, '-r', 'file_put_contents($argv[1], $argv[2]);', $marker, $spaced]);
check($rc2 === 0 && file_get_contents($marker) === $spaced, 'spaced argv intact');

// A quote/injection-shaped arg is inert (no shell involved).
$evil = "x'; touch " . sys_get_temp_dir() . "/wdf-pwned3; echo '";
[$rc3, ] = wdf_run_demo([PHP_BINARY, '-r', 'file_put_contents($argv[1], $argv[2]);', $marker, $evil]);
check($rc3 === 0 && file_get_contents($marker) === $evil, 'quote-shaped argv passed as data');
check(!file_exists(sys_get_temp_dir() . '/wdf-pwned3'), 'no injection side effect');

// Shell fallback path (called directly; wdf_run_demo uses proc_open on this
// PHP): Hebrew argv survive because a UTF-8 locale is selectable here. The
// fail-closed branch needs a PHP without any UTF-8 locale, which this test
// host has, so it is exercised by review rather than at runtime.
setlocale(LC_CTYPE, 'C');
[$rc4, ] = wdf_run_demo_shell([PHP_BINARY, '-r', 'file_put_contents($argv[1], $argv[2]);', $marker, $hebrew]);
check($rc4 === 0 && file_get_contents($marker) === $hebrew, 'shell fallback: Hebrew argv intact with UTF-8 locale');

// >64KB of stderr (past the pipe buffer) must not deadlock the runner.
[$rc5, $lines5] = wdf_run_demo([
    PHP_BINARY, '-r',
    'fwrite(STDERR, str_repeat("E", 200000) . "\n"); echo "after-flood\n";',
]);
check($rc5 === 0, 'no deadlock past the 64KB pipe buffer');
check(in_array('after-flood', $lines5, true), 'stdout captured after stderr flood');
check(strlen(implode("\n", $lines5)) > 200000, 'full stderr flood captured');

// stdout+stderr are both captured.
check(in_array('rc-line', $lines, true) && in_array('err-line', $lines, true), 'stdout and stderr captured');

unlink($marker);
if ($fails > 0) {
    fwrite(STDERR, "{$fails} test(s) failed\n");
    exit(1);
}
echo "exec_test: all assertions passed\n";
