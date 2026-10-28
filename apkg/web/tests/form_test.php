<?php
// Demo form markup tests (issue #29): the PIN field must not trigger the
// browser password manager. Safari ignores autocomplete="off" on password
// fields but honors autocomplete="one-time-code" (one-time code: no saved-
// password offers, no save prompt). The PIN stays masked (type=password).
// Run: php apkg/web/tests/form_test.php

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

$src = file_get_contents(__DIR__ . '/../index.php');
check($src !== false, 'index.php readable');

// Locate the pin input tag specifically.
check(preg_match('/<input\b[^>]*\bname="pin"[^>]*>/s', $src, $m) === 1, 'pin input present');
$tag = $m[0] ?? '';
check(strpos($tag, 'type="password"') !== false, 'pin stays masked (type=password)');
check(strpos($tag, 'autocomplete="one-time-code"') !== false, 'pin opts out of password manager (one-time-code)');
check(strpos($tag, 'autocomplete="off"') === false, 'no useless autocomplete="off" (Safari ignores it)');
check(strpos($tag, 'autocapitalize="off"') !== false, 'no autocapitalize on mobile keyboards');
check(strpos($tag, 'autocorrect="off"') !== false, 'no autocorrect');
check(strpos($tag, 'spellcheck="false"') !== false, 'no spellcheck');
check(strpos($tag, 'required') !== false, 'pin still required');

// The form is still the only POST path and still CSRF-protected.
check(strpos($src, 'name="csrf"') !== false, 'csrf token field still present');

if ($fails > 0) {
    fwrite(STDERR, "{$fails} test(s) failed\n");
    exit(1);
}
echo "form_test: all assertions passed\n";
