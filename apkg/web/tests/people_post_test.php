<?php
// Handler-level rename test (issue #95, CodeRabbit #89 follow-up): the
// people.php POST handler must pass the TRIMMED name to wdf_faces
// --name. The handler runs in a CHILD process (people_post_child.php):
// its success path calls exit, which would otherwise skip these
// assertions silently (CodeRabbit #96). The stub wdf_faces records its
// argv; the parent asserts --name carries the trimmed value.
// Run: php apkg/web/tests/people_post_test.php

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

// Unpredictable 0700 temp dir (CodeRabbit #96): a PID-based name is a
// symlink target on shared machines.
$tmp = tempnam(sys_get_temp_dir(), 'wdf-pp-');
if ($tmp === false || !unlink($tmp) || !mkdir($tmp, 0700)) {
    fwrite(STDERR, "cannot create temp dir\n");
    exit(1);
}
mkdir($tmp . '/app/bin', 0700, true);
mkdir($tmp . '/sess', 0700, true);

// Stub wdf_faces: log argv (one per line), serve an empty people list,
// succeed on mutations so the handler takes its PRG exit path.
$argsLog = $tmp . '/args.log';
file_put_contents($tmp . '/app/bin/wdf_faces',
    "#!/bin/sh\n"
    . "printf '%s\n' \"\$@\" >> \"\$WDF_ARGS_LOG\"\n"
    . "if [ \"\$1\" = people ]; then echo '[]'; fi\n"
    . "exit 0\n");
chmod($tmp . '/app/bin/wdf_faces', 0700);

file_put_contents($tmp . '/demo-pin.txt', "0123abcd4567\n");
file_put_contents($tmp . '/wfi.conf', "data_dir={$tmp}/data\nroots=/photos\n");

putenv('WDF_APP_DIR=' . $tmp . '/app');
putenv('WDF_PIN_FILE=' . $tmp . '/demo-pin.txt');
putenv('WDF_WFI_CONFIG=' . $tmp . '/wfi.conf');
putenv('WDF_ARGS_LOG=' . $argsLog);

$desc = [1 => ['file', $tmp . '/child.out', 'w'],
         2 => ['file', $tmp . '/child.err', 'w']];
$proc = proc_open(
    [PHP_BINARY, __DIR__ . '/people_post_child.php', $tmp], $desc, $pipes);
check(is_resource($proc), 'child process launches');
$rc = is_resource($proc) ? proc_close($proc) : -1;
check($rc === 0, 'child exits 0 (rename accepted, PRG path)');

$log = is_file($argsLog) ? (string)file_get_contents($argsLog) : '';
check(strpos($log, "rename\n") !== false, 'rename reached the stub');
check(strpos($log, "--name\nדנה\n") !== false,
    '--name carries the trimmed name');
check(strpos($log, "  דנה") === false && strpos($log, "דנה\u{00A0}") === false,
    'no untrimmed form reaches the CLI');

// Cleanup (best effort).
foreach ([$tmp . '/app/bin/wdf_faces', $tmp . '/demo-pin.txt',
          $tmp . '/wfi.conf', $tmp . '/args.log', $tmp . '/child.out',
          $tmp . '/child.err'] as $f) {
    @unlink($f);
}
foreach (glob($tmp . '/sess/*') ?: [] as $f) {
    @unlink($f);
}
@rmdir($tmp . '/app/bin'); @rmdir($tmp . '/app'); @rmdir($tmp . '/sess');
@rmdir($tmp);

if ($fails > 0) {
    fwrite(STDERR, "{$fails} check(s) failed\n");
    exit(1);
}
echo "people_post_test: all checks passed\n";
