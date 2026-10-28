<?php
// Demo process runner (issue #24, Hebrew-path follow-up).
// escapeshellarg() strips bytes that are invalid in the web SAPI's LC_CTYPE,
// which on a minimal NAS is typically "C" - a Hebrew path like
// /shares/תמונות reaches the demo as "/shares//". PHP >= 7.4 lets proc_open
// take an argv array and execvp()s it directly: no shell, no locale, raw
// bytes, and no injection surface. The fallback keeps the old shell path with
// a best-effort UTF-8 locale for ancient PHP.

// Runs $argv (array, first element the binary) and returns [exit_code, lines].
function wdf_run_demo(array $argv): array
{
    if (PHP_VERSION_ID >= 70400) {
        // Both streams go to one temp file, not pipes: reading stdout to EOF
        // before stderr deadlocks once the child fills the 64KB pipe buffer
        // (many undecodable JPEGs = a long stderr), and the demo page holds
        // an exclusive lock while it waits. A file has no buffer limit.
        $tmp = tempnam(sys_get_temp_dir(), 'wdf-demo-');
        if ($tmp === false) {
            return [127, ['failed to create a temp file for demo output']];
        }
        $desc = [1 => ['file', $tmp, 'a'], 2 => ['file', $tmp, 'a']];
        $proc = proc_open($argv, $desc, $pipes);
        if (!is_resource($proc)) {
            @unlink($tmp);
            return [127, ['failed to start: ' . $argv[0]]];
        }
        $rc = proc_close($proc);
        $text = trim((string)file_get_contents($tmp));
        unlink($tmp);
        return [$rc, $text === '' ? [] : explode("\n", $text)];
    }
    return wdf_run_demo_shell($argv);
}

// Shell fallback for PHP < 7.4. escapeshellarg() strips bytes that are
// invalid in the active LC_CTYPE, so: ASCII args pass untouched under any
// locale; non-ASCII args need a working UTF-8 locale, and if none can be
// selected we fail closed with a clear message instead of letting the demo
// receive a silently mangled path.
function wdf_run_demo_shell(array $argv): array
{
    $needsUtf8 = false;
    foreach ($argv as $a) {
        if (preg_match('/[^\x00-\x7F]/', $a) === 1) {
            $needsUtf8 = true;
            break;
        }
    }
    if ($needsUtf8
        && setlocale(LC_CTYPE, 'C.UTF-8') === false
        && setlocale(LC_CTYPE, 'en_US.UTF-8') === false) {
        return [1, ['This server (PHP ' . PHP_VERSION . ') cannot pass '
            . 'non-ASCII folder names to the demo: proc_open argv arrays need '
            . 'PHP >= 7.4 and no UTF-8 locale is installed. Use an ASCII '
            . 'folder name, or report this so the app can be rebuilt.']];
    }
    $cmd = implode(' ', array_map('escapeshellarg', $argv)) . ' 2>&1';
    exec($cmd, $lines, $rc);
    return [$rc, $lines];
}
