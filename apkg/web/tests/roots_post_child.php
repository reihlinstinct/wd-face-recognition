<?php
// Child runner for roots_post_test.php (issue #102): drives the real
// index.php through GET (seeds the session CSRF token) then three POSTs
// - a valid save, an invalid root, and a wrong PIN. Runs in its own
// process because the handler's success path ends with exit; the PARENT
// asserts the resulting wfi.conf after each phase. Phases are separated
// by marker files the parent creates.
$tmp = $argv[1];
session_save_path($tmp . '/sess');
$_SERVER['REQUEST_METHOD'] = 'GET';
$_SERVER['REQUEST_URI'] = '/index.php';
ob_start();
include dirname(__DIR__) . '/index.php';
ob_end_clean();
$csrf = $_SESSION['csrf'] ?? '';
if ($csrf === '') {
    fwrite(STDERR, "no csrf token seeded\n");
    exit(1);
}
file_put_contents($tmp . '/ready', '1');

// Order matters: a correct PIN MARKS the session (remembered browser),
// so the wrong-PIN phase must run FIRST, before any correct PIN; and a
// successful save ends with the PRG exit, so the valid POST is LAST.
$phases = [
    'go-badpin' => [
        'pin' => 'wrong-pin-0000',
        'roots' => $GLOBALS['argv'][3],
    ],
    'go-invalid' => [
        'pin' => '0123abcd4567',
        'roots' => $GLOBALS['argv'][2] . "\n" . $GLOBALS['argv'][4],  // missing dir
    ],
    'go-valid' => [
        'pin' => '0123abcd4567',
        'roots' => $GLOBALS['argv'][2] . "\n" . $GLOBALS['argv'][3],
    ],
];
// The success path of index.php ends with exit (PRG): mark the running
// phase done from a shutdown function so the parent unblocks either way.
$currentGate = '';
register_shutdown_function(function () use ($tmp, &$currentGate) {
    if ($currentGate !== '') {
        file_put_contents($tmp . '/' . $currentGate . '-done', '1');
    }
});
foreach ($phases as $gate => $cfg) {
    while (!is_file($tmp . '/' . $gate)) {
        usleep(20000);
    }
    $_SERVER['REQUEST_METHOD'] = 'POST';
    $_POST = [
        'pin' => $cfg['pin'],
        'csrf' => $csrf,
        'action' => 'set_roots',
        'roots' => $cfg['roots'],
    ];
    $currentGate = $gate;
    ob_start();
    include dirname(__DIR__) . '/index.php';
    ob_end_clean();
    $currentGate = '';
    file_put_contents($tmp . '/' . $gate . '-done', '1');
}
