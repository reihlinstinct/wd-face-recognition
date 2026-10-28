<?php
// Session-auth tests (CodeRabbit PR #42): the remembered browser is bound
// to the current PIN (rotation invalidates), a missing PIN file
// invalidates, and HTTPS detection drives cookie persistence.
// Run: php apkg/web/tests/auth_test.php
require_once __DIR__ . '/../auth.php';

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

// wdf_request_is_https: header, forwarded proto, and port variants.
$_SERVER = [];
check(!wdf_request_is_https(), 'no markers = not https');
$_SERVER = ['HTTPS' => 'on'];
check(wdf_request_is_https(), 'HTTPS=on detected');
$_SERVER = ['HTTPS' => 'off'];
check(!wdf_request_is_https(), 'HTTPS=off is not https');
$_SERVER = ['HTTP_X_FORWARDED_PROTO' => 'https'];
check(wdf_request_is_https(), 'forwarded proto detected');
$_SERVER = ['HTTP_X_FORWARDED_PROTO' => 'http', 'SERVER_PORT' => '443'];
check(wdf_request_is_https(), 'port 443 detected');
$_SERVER = ['SERVER_PORT' => '80'];
check(!wdf_request_is_https(), 'port 80 is not https');
$_SERVER = [];

// wdf_pin_session_ok / wdf_pin_session_mark against a temp PIN file.
if (session_status() !== PHP_SESSION_ACTIVE) {
    session_start();
}
$pinFile = sys_get_temp_dir() . '/wdf-auth-' . getmypid() . '.pin';
file_put_contents($pinFile, "0123abcd4567\n");

$_SESSION = [];
check(!wdf_pin_session_ok($pinFile), 'no session flag = not ok');

$sidBefore = session_id();
wdf_pin_session_mark('0123abcd4567');
check(wdf_pin_session_ok($pinFile), 'marked session is ok for the same PIN');
check(session_id() !== $sidBefore, 'mark regenerates the session id (fixation)');

// PIN rotation: remembered browser is invalidated.
file_put_contents($pinFile, "ffffffffffff\n");
check(!wdf_pin_session_ok($pinFile), 'PIN rotation invalidates the remembered browser');
check(empty($_SESSION['pin_ok']), 'invalidation clears the session flag');

// Missing PIN file invalidates too.
wdf_pin_session_mark('0123abcd4567');
unlink($pinFile);
check(!wdf_pin_session_ok($pinFile), 'missing PIN file invalidates');

if ($fails > 0) {
    fwrite(STDERR, "{$fails} test(s) failed\n");
    exit(1);
}
echo "auth_test: all assertions passed\n";
