<?php
// Child runner for people_post_test.php (issue #95): drives the real
// people.php through GET (seeds the session CSRF token) then POST
// (rename with a space+NBSP-wrapped name). Runs in its own process
// because the handler's success path ends with exit - assertions on
// the recorded argv happen in the PARENT, so an early exit here can
// never skip them.
$tmp = $argv[1];
session_save_path($tmp . '/sess');
$_SERVER['REQUEST_METHOD'] = 'GET';
ob_start();
include dirname(__DIR__) . '/people.php';
ob_end_clean();
$csrf = $_SESSION['csrf'] ?? '';
$_SERVER['REQUEST_METHOD'] = 'POST';
$_POST = [
    'pin' => '0123abcd4567',
    'csrf' => $csrf,
    'action' => 'rename',
    'id' => '1',
    'name' => "  דנה\u{00A0}",
];
ob_start();
include dirname(__DIR__) . '/people.php';
ob_end_clean();
