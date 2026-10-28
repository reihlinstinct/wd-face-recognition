<?php
// Demo run status, parsed from the async run's log file (issue #33).
// The async wrapper appends one machine line, "exit=<rc>", when the demo
// finishes, so a log without that line is a run still in progress.

// Returns ['state' => idle|running|done|failed, 'total' => ?int,
//          'processed' => int, 'exit' => ?int, 'start' => ?int,
//          'pid' => ?int, 'tail' => string[]]
function wdf_demo_status(string $logFile): array
{
    $st = ['state' => 'idle', 'total' => null, 'processed' => 0,
           'exit' => null, 'start' => null, 'pid' => null, 'tail' => []];
    if (!is_file($logFile)) {
        return $st;
    }
    $lines = file($logFile, FILE_IGNORE_NEW_LINES);
    if ($lines === false) {
        return $st;
    }
    foreach ($lines as $line) {
        if ($st['total'] === null && preg_match('/^photos=(\d+)$/', $line, $m) === 1) {
            $st['total'] = (int)$m[1];
        } elseif (preg_match('/^exit=(\d+)$/', $line, $m) === 1) {
            $st['exit'] = (int)$m[1];
        } elseif ($st['start'] === null && preg_match('/^start=(\d+)$/', $line, $m) === 1) {
            $st['start'] = (int)$m[1];
        } elseif ($st['pid'] === null && preg_match('/^pid=(\d+)$/', $line, $m) === 1) {
            $st['pid'] = (int)$m[1];
        } elseif (preg_match('/: faces=\d+$/', $line) === 1) {
            $st['processed']++;
        }
    }
    $st['tail'] = array_slice($lines, -30);
    if ($st['exit'] === null) {
        $st['state'] = 'running';
    } else {
        $st['state'] = $st['exit'] === 0 ? 'done' : 'failed';
    }
    return $st;
}

// Estimated seconds left, from elapsed time and the processed/total ratio.
// Needs a real rate: at least 2 processed photos and a known start time.
function wdf_demo_eta(array $st, int $now): ?int
{
    if ($st['state'] !== 'running' || $st['total'] === null
        || $st['processed'] < 2 || $st['start'] === null) {
        return null;
    }
    $elapsed = $now - $st['start'];
    if ($elapsed <= 0) {
        return null;
    }
    $rate = $st['processed'] / $elapsed;
    return (int)round(($st['total'] - $st['processed']) / $rate);
}

// True only while $pid is alive AND belongs to the demo binary (checked via
// /proc/<pid>/cmdline). The identity match matters twice: a wrapper killed
// before writing "exit=" (crash, NAS reboot) must not leave the run
// "running" forever, and a logged PID reused by an unrelated process must
// never receive the cancel kill (CodeRabbit PR #34 review).
function wdf_demo_pid_is_demo(?int $pid, string $needle = 'wdf_demo'): bool
{
    if ($pid === null || $pid <= 0) {
        return false;
    }
    $cmd = @file_get_contents('/proc/' . $pid . '/cmdline');
    return $cmd !== false && strpos($cmd, $needle) !== false;
}

// Parsed status with liveness applied: a run whose recorded demo process is
// gone (or whose PID now belongs to something else) has failed orphaned -
// the exit marker never came. The startup window (start= written, pid= not
// yet) still counts as running.
function wdf_demo_status_live(string $logFile): array
{
    $st = wdf_demo_status($logFile);
    if ($st['state'] === 'running' && $st['pid'] !== null
        && !wdf_demo_pid_is_demo($st['pid'])) {
        $st['state'] = 'failed';
        $st['orphaned'] = true;
    }
    return $st;
}
