<?php
// Demo form markup tests (issue #29): the PIN field must not trigger the
// browser password manager. Safari ignores autocomplete="off" on password
// fields but honors autocomplete="one-time-code" (one-time code: no saved-
// password offers, no save prompt). The PIN stays masked (type=password).
//
// The test renders the real GET page (output buffering, CLI SAPI) and
// parses the rendered pin input's attributes exactly, so a PHP conditional
// or a lookalike attribute (data-type=, aria-required=) cannot fake a pass.
// Run: php apkg/web/tests/form_test.php

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

// Render the real page with a GET request. index.php only starts a session
// and emits HTML on GET; the demo POST path is not exercised here.
$sessDir = sys_get_temp_dir() . '/wdf-form-' . getmypid();
@mkdir($sessDir, 0777, true);
session_save_path($sessDir);
$_SERVER['REQUEST_METHOD'] = 'GET';
ob_start();
include __DIR__ . '/../index.php';
$html = ob_get_clean();

check(is_string($html) && strpos($html, '<form method="post">') !== false, 'GET renders the demo form');

// Locate the rendered pin input tag and parse its attributes exactly.
check(preg_match('/<input\b[^>]*\bname="pin"[^>]*>/s', (string)$html, $m) === 1, 'pin input rendered');
$tag = $m[0] ?? '';
preg_match_all('/([\w-]+)\s*=\s*"([^"]*)"/s', $tag, $am, PREG_SET_ORDER);
$attrs = [];
foreach ($am as $a) {
    $attrs[$a[1]] = $a[2];
}
check(($attrs['type'] ?? '') === 'password', 'pin stays masked (type=password, exact attribute)');
check(($attrs['autocomplete'] ?? '') === 'one-time-code', 'pin opts out of password manager (one-time-code, exact attribute)');
check(($attrs['autocapitalize'] ?? '') === 'off', 'no autocapitalize on mobile keyboards');
check(($attrs['autocorrect'] ?? '') === 'off', 'no autocorrect');
check(($attrs['spellcheck'] ?? '') === 'false', 'no spellcheck');
check(preg_match('/\srequired(?=[\s>\/])/', $tag) === 1, 'pin still required (bare attribute, not aria-*)');

// The form still POSTs and is still CSRF-protected.
check(preg_match('/<input\b[^>]*\bname="csrf"[^>]*>/s', (string)$html) === 1, 'csrf token field rendered');

// PIN discovery hint (issue #32): concrete SMB example + Hebrew line, no
// dead-end "dashboard file manager" reference.
check(strpos((string)$html, 'facerecog-reports') !== false, 'hint names the reports share');
check(strpos((string)$html, '\\\\') !== false, 'hint shows an SMB-style path');
check(strpos((string)$html, 'demo-pin.txt') !== false, 'hint names the PIN file');
check(strpos((string)$html, 'dashboard file manager') === false, 'dead-end dashboard-file-manager wording gone');
check(strpos((string)$html, 'קוד ה-PIN') !== false, 'Hebrew hint line present');

// Recovery path (CodeRabbit PR #34): the hint must say what to do when the
// share or PIN file is missing - toggle the app off/on to regenerate both.
check(strpos((string)$html, 'toggle the app off and back on') !== false,
      'hint explains the disable/re-enable recovery (EN)');
check(strpos((string)$html, 'ייווצר PIN חדש') !== false,
      'Hebrew hint line explains the recovery too');

if ($fails > 0) {
    fwrite(STDERR, "{$fails} test(s) failed\n");
    exit(1);
}
echo "form_test: all assertions passed\n";
