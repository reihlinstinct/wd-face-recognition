<?php
// In-browser demo results (issue #35): serves demo-out/index.html with the
// crop URLs rewritten through this endpoint, so the grouped contact sheet
// opens straight from the app page - no SMB trip. Grouped faces are
// private: the page requires the demo PIN once per session (same PIN as
// the run form; a successful run/cancel POST also unlocks it).
require_once __DIR__ . '/auth.php';
wdf_session_start();
require_once __DIR__ . '/paths.php';
require_once __DIR__ . '/settings.php';

$reportsDir = '/shares/facerecog-reports';
// Configurable output folder (issue #43): results are served from the
// folder chosen on the app page (validated there; this stays a read).
$demoOut = wdf_output_dir($reportsDir);
$pinFile = getenv('WDF_PIN_FILE') !== false ? (string)getenv('WDF_PIN_FILE') : $reportsDir . '/demo-pin.txt';

// Auth comes BEFORE any content: the crops are the user's private face
// photos, so the image endpoint is gated exactly like the page
// (CodeRabbit PR #42), and the remembered session must match the
// current PIN (rotation invalidates it).
if (!wdf_pin_session_ok($pinFile) && isset($_GET['img'])) {
    http_response_code(403);
    exit;
}

// Image endpoint: basename-only, resolved inside the crops directory.
// no-store: PIN-gated content must not linger in any cache.
if (isset($_GET['img'])) {
    $crop = wdf_safe_crop_path($demoOut . '/crops', (string)$_GET['img']);
    if ($crop === null) {
        http_response_code(404);
        exit;
    }
    header('Content-Type: image/jpeg');
    header('Cache-Control: no-store');
    readfile($crop);
    exit;
}

$msg = '';
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $pin = trim((string)($_POST['pin'] ?? ''));
    $expectedPin = is_file($pinFile) ? trim((string)file_get_contents($pinFile)) : '';
    if ($expectedPin !== '' && hash_equals($expectedPin, $pin)) {
        wdf_pin_session_mark($expectedPin);
        header('Location: results.php', true, 303);
        exit;
    }
    $msg = 'Wrong demo PIN.';
}

header('Content-Type: text/html; charset=utf-8');
header('Cache-Control: no-store');
// The served index.html lives on a user-writable share (issue #43): anyone
// with share access could inject markup/JS into this origin. The generated
// page is fully static, so forbid everything but same-origin images and
// inline styles - injected scripts, forms and frames simply never run
// (CodeRabbit PR #44).
header("Content-Security-Policy: default-src 'none'; img-src 'self'; style-src 'unsafe-inline'");

if (!wdf_pin_session_ok($pinFile)) {
    ?>
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WD Face Recognition - results</title>
<style>
body{font-family:system-ui;margin:16px;max-width:40em}
.msg{padding:10px;background:#f2e3e3;border-radius:6px}
.hint{font-size:.9em;color:#444}
label{font-weight:600}
</style>
</head>
<body>
<h1>Demo results</h1>
<?php if ($msg !== ''): ?>
<p class="msg"><?= htmlspecialchars($msg, ENT_QUOTES, 'UTF-8') ?></p>
<?php endif; ?>
<form method="post">
<label for="pin">Demo PIN</label> (the results show your photos; first entry on this browser is remembered for 30 days):<br>
<span class="hint">\\NAS\facerecog-reports\demo-pin.txt - קוד ה-PIN בשיתוף facerecog-reports.</span><br>
<input type="password" id="pin" name="pin" autocomplete="one-time-code"
       autocapitalize="off" autocorrect="off" spellcheck="false" required>
<button type="submit">Show results</button>
</form>
<p><a href="index.php">Back to the demo page</a></p>
</body>
</html>
<?php
    exit;
}

$htmlFile = $demoOut . '/index.html';
if (!is_file($htmlFile)) {
    ?>
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WD Face Recognition - results</title>
<style>body{font-family:system-ui;margin:16px;max-width:40em}</style>
</head>
<body>
<h1>Demo results</h1>
<p>No results yet - run the demo first. <a href="index.php">Back to the demo page</a></p>
</body>
</html>
<?php
    exit;
}

// The generated page references crops as relative "crops/fN.jpg"; rewrite
// them through this endpoint, which validates basename + containment.
$html = (string)file_get_contents($htmlFile);
$html = preg_replace("#src='crops/([^']+)'#", "src='results.php?img=\$1'", $html);
echo $html;
