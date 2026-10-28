<?php
// Tests for the demo run status parsing + ETA (issue #33).
// Run: php apkg/web/tests/status_test.php
require_once __DIR__ . '/../status.php';

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

$tmp = sys_get_temp_dir() . '/wdf-status-' . getmypid();
@mkdir($tmp, 0777, true);

// No log yet: idle.
$st = wdf_demo_status($tmp . '/missing.log');
check($st['state'] === 'idle', 'no log = idle');

// Mid-run log: total known, two photos processed, no exit marker.
$log = $tmp . '/run.log';
file_put_contents($log, implode("\n", [
    'start=1000',
    'pid=4321',
    'photos=4',
    'note: folders deeper than 8 levels were skipped',
    'a.jpg: faces=2',
    'תמונה ב.jpg: faces=0',
]) . "\n");
$st = wdf_demo_status($log);
check($st['state'] === 'running', 'no exit marker = running');
check($st['total'] === 4, 'total parsed');
check($st['processed'] === 2, 'processed counts face lines (Hebrew name included)');
check($st['start'] === 1000, 'start parsed');
check($st['pid'] === 4321, 'pid parsed');

// ETA: 2 photos in 100s -> 2 remaining -> ~100s.
$eta = wdf_demo_status($log) + [];
check(wdf_demo_eta($st, 1100) === 100, 'ETA from rate');
check(wdf_demo_eta($st, 1000) === null, 'ETA null at zero elapsed');
$st0 = $st; $st0['processed'] = 1;
check(wdf_demo_eta($st0, 1100) === null, 'ETA needs >= 2 processed');
$stDone = $st; $stDone['state'] = 'done';
check(wdf_demo_eta($stDone, 1100) === null, 'no ETA when not running');

// Finished OK.
file_put_contents($log, "start=1000\npid=4321\nphotos=1\nx.jpg: faces=1\nfaces=1 people=1 -> /out/index.html\nexit=0\n");
$st = wdf_demo_status($log);
check($st['state'] === 'done' && $st['exit'] === 0, 'exit=0 = done');
check($st['processed'] === 1, 'done processed count');

// Failed run: exit marker non-zero.
file_put_contents($log, "start=1000\npid=4321\ncannot open /shares/nope\nexit=1\n");
$st = wdf_demo_status($log);
check($st['state'] === 'failed' && $st['exit'] === 1, 'exit=1 = failed');
check(in_array('cannot open /shares/nope', $st['tail'], true), 'tail carries log lines');

// Cancelled run: SIGTERM shows up as exit=143, still "failed" (not stuck).
file_put_contents($log, "start=1000\npid=4321\nphotos=9\nexit=143\n");
$st = wdf_demo_status($log);
check($st['state'] === 'failed' && $st['exit'] === 143, 'killed run settles as failed, not running');

if ($fails > 0) {
    fwrite(STDERR, "{$fails} test(s) failed\n");
    exit(1);
}
echo "status_test: all assertions passed\n";
