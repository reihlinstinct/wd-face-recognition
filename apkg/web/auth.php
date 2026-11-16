<?php
// Session bootstrap (issue #35 follow-up): one correct PIN entry is
// remembered per browser for 30 days via a long-lived session cookie,
// instead of re-typing the PIN on every visit. HttpOnly + SameSite=Lax;
// Secure is off because the NAS dashboard is commonly plain http on LANs.
function wdf_session_start(): void
{
    if (session_status() === PHP_SESSION_ACTIVE) {
        return;
    }
    $month = 30 * 24 * 60 * 60;
    $https = wdf_request_is_https();
    session_set_cookie_params([
        // 30 days over HTTPS; browser-session over plain HTTP.
        'lifetime' => $https ? $month : 0,
        'path' => '/',
        'secure' => $https,
        'httponly' => true,
        'samesite' => 'Lax',
    ]);
    ini_set('session.gc_maxlifetime', (string)$month);
    session_start();
}

// HTTPS detection: a 30-day cookie over plain HTTP would cross the LAN
// readable by any on-path device (CodeRabbit PR #42). Over HTTP the session
// cookie falls back to browser-session lifetime (no persistent remember).
function wdf_request_is_https(): bool
{
    if (!empty($_SERVER['HTTPS']) && strtolower((string)$_SERVER['HTTPS']) !== 'off') {
        return true;
    }
    if (strtolower((string)($_SERVER['HTTP_X_FORWARDED_PROTO'] ?? '')) === 'https') {
        return true;
    }
    return (int)($_SERVER['SERVER_PORT'] ?? 0) === 443;
}

// The remembered session is bound to the CURRENT PIN: toggling the app
// (PIN rotation) invalidates every remembered browser (CodeRabbit PR #42).
function wdf_pin_session_ok(string $pinFile): bool
{
    if (empty($_SESSION['pin_ok'])) {
        return false;
    }
    $expectedPin = is_file($pinFile) ? trim((string)file_get_contents($pinFile)) : '';
    $key = $expectedPin === '' ? '' : hash('sha256', $expectedPin);
    if ($key === '' || !hash_equals((string)($_SESSION['pin_key'] ?? ' '), $key)) {
        unset($_SESSION['pin_ok'], $_SESSION['pin_key']);
        return false;
    }
    return true;
}

// Call this when a PIN was just verified in this request.
function wdf_pin_session_mark(string $pin): void
{
    // Session-fixation guard (CodeRabbit PR #42): a planted session id must
    // not become authorized when the victim enters the PIN - rotate the id
    // on the unauthenticated -> authenticated transition.
    if (session_status() === PHP_SESSION_ACTIVE) {
        session_regenerate_id(true);
    }
    $_SESSION['pin_ok'] = true;
    $_SESSION['pin_key'] = hash('sha256', $pin);
}
