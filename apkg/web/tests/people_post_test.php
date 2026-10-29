<?php
// Handler-level rename test (issue #95, CodeRabbit #89 follow-up): the
// people.php POST handler must pass the TRIMMED name to wdf_faces
// --name. The stub records its argv; a space+NBSP-wrapped name must
// arrive trimmed. The stub exits 1 on mutations so the handler renders
// instead of taking its PRG exit, letting the include return.
// Run: php apkg/web/tests/people_post_test.php

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

$tmp = sys_get_temp_dir() . '/wdf-people-post-' . getmypid();
@mkdir($tmp . '/app/bin', 0777, true);
@mkdir($tmp . '/sess', 0777, true);

// Stub wdf_faces: log argv (one per line), serve an empty people list,
// fail mutations so the handler skips its PRG exit.
$argsLog = $tmp . '/args.log';
file_put_contents($tmp . '/app/bin/wdf_faces',
    "#!/bin/sh\n"
    . "printf '%s\n' \"\$@\" >> \"\$WDF_ARGS_LOG\"\n"
    . "if [ \"\$1\" = people ]; then echo '[]'; exit 0; fi\n"
    . "exit 1\n");
chmod($tmp . '/app/bin/wdf_faces', 0755);

$pinFile = $tmp . '/demo-pin.txt';
file_put_contents($pinFile, "0123abcd4567\n");
$conf = $tmp . '/wfi.conf';
file_put_contents($conf, "data_dir={$tmp}/data\nroots=/photos\n");

putenv('WDF_APP_DIR=' . $tmp . '/app');
putenv('WDF_PIN_FILE=' . $pinFile);
putenv('WDF_WFI_CONFIG=' . $conf);
putenv('WDF_ARGS_LOG=' . $argsLog);

session_save_path($tmp . '/sess');

// GET: seeds the session CSRF token (and exercises the list path).
$_SERVER['REQUEST_METHOD'] = 'GET';
ob_start();
include __DIR__ . '/../people.php';
ob_end_clean();
$csrf = $_SESSION['csrf'] ?? '';
check($csrf !== '', 'GET seeds the session CSRF token');

// POST: rename wrapped in spaces and a trailing NBSP (U+00A0).
$_SERVER['REQUEST_METHOD'] = 'POST';
$_POST = [
    'pin' => '0123abcd4567',
    'csrf' => $csrf,
    'action' => 'rename',
    'id' => '1',
    'name' => "  דנה\u{00A0}",
];
ob_start();
include __DIR__ . '/../people.php';
ob_end_clean();

$log = is_file($argsLog) ? (string)file_get_contents($argsLog) : '';
check(strpos($log, "rename\n") !== false, 'rename reached the stub');
check(strpos($log, "--name\nדנה\n") !== false,
    '--name carries the trimmed name');
check(strpos($log, "  דנה") === false && strpos($log, "דנה\u{00A0}") === false,
    'no untrimmed form reaches the CLI');

if ($fails > 0) {
    fwrite(STDERR, "{$fails} check(s) failed\n");
    exit(1);
}
echo "people_post_test: all checks passed\n";
