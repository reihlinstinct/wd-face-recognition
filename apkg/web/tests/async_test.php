<?php
// Tests for the detached async demo launch (issue #33). The launch must
// return immediately while the job keeps running detached, write machine
// lines (start/pid/exit) around the output, and pass non-ASCII (Hebrew)
// arguments byte-for-byte. Run: php apkg/web/tests/async_test.php
require_once __DIR__ . '/../run.php';
require_once __DIR__ . '/../status.php';

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

$tmp = sys_get_temp_dir() . '/wdf-async-' . getmypid();
@mkdir($tmp, 0777, true);
$log = $tmp . '/run.log';

// Launch a stub "demo" that sleeps, then emits the usual lines.
$t0 = microtime(true);
$ok = wdf_run_demo_async(
    ['/bin/sh', '-c', 'sleep 0.5; echo "photos=2"; echo "a.jpg: faces=1"; echo "b.jpg: faces=0"'],
    $log
);
$launchMs = (microtime(true) - $t0) * 1000;
check($ok, 'async launch accepted');
check($launchMs < 500, 'launch returns immediately, does not wait for the job');

// Right after launch the job is still running (or just flipping to done on
// a very fast machine): the log must at least exist shortly.
$deadline = microtime(true) + 5;
$st = null;
while (microtime(true) < $deadline) {
    $st = wdf_demo_status($log);
    if ($st['state'] === 'done') {
        break;
    }
    usleep(100000);
}
check($st !== null && $st['state'] === 'done', 'detached job ran to completion');
check($st['exit'] === 0, 'exit line appended with rc 0');
check($st['start'] !== null && $st['start'] > 0, 'start line written');
check($st['pid'] !== null && $st['pid'] > 0, 'demo pid recorded');
check($st['total'] === 2 && $st['processed'] === 2, 'output lines parsed');

// Non-zero exit propagates through the wrapper.
$log2 = $tmp . '/run2.log';
check(wdf_run_demo_async(['/bin/sh', '-c', 'exit 3'], $log2), 'second launch accepted');
$deadline = microtime(true) + 5;
$st = null;
while (microtime(true) < $deadline) {
    $st = wdf_demo_status($log2);
    if ($st['state'] === 'failed') {
        break;
    }
    usleep(100000);
}
check($st !== null && $st['exit'] === 3, 'non-zero exit recorded');

// Hebrew argument arrives byte-for-byte (no locale/escaping loss).
$log3 = $tmp . '/run3.log';
wdf_run_demo_async(['/bin/sh', '-c', 'echo "folder=$1"', 'sh', '/shares/תמונות חופשה'], $log3);
$deadline = microtime(true) + 5;
$found = false;
while (microtime(true) < $deadline) {
    if (is_file($log3) && strpos((string)file_get_contents($log3), 'folder=/shares/תמונות חופשה') !== false) {
        $found = true;
        break;
    }
    usleep(100000);
}
check($found, 'Hebrew argument passed byte-for-byte');

if ($fails > 0) {
    fwrite(STDERR, "{$fails} test(s) failed\n");
    exit(1);
}
echo "async_test: all assertions passed\n";
