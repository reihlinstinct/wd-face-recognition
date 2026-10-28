<?php
// Person page (issue #50, 0.3.0 PR 3d): every face the index assigned to
// one cluster, with the photo each crop came from. PIN-gated like the
// other private pages.
require_once __DIR__ . '/auth.php';
wdf_session_start();
require_once __DIR__ . '/paths.php';
require_once __DIR__ . '/people_lib.php';

$appDir = realpath(dirname(__DIR__));
$configPath = wdf_wfi_config_path($appDir);

$reportsDir = '/shares/facerecog-reports';
$pinOverride = getenv('WDF_PIN_FILE');
$pinFile = ($pinOverride !== false && $pinOverride !== '')
    ? $pinOverride : $reportsDir . '/demo-pin.txt';
$expectedPin = is_readable($pinFile)
    ? trim((string)file_get_contents($pinFile)) : '';

$msg = '';
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $pin = (string)($_POST['pin'] ?? '');
    if ($expectedPin !== '' && hash_equals($expectedPin, $pin)) {
        wdf_pin_session_mark($expectedPin);
        header('Location: ' . $_SERVER['REQUEST_URI'], true, 303);
        exit;
    }
    $msg = 'Wrong demo PIN.';
}

$id = wdf_post_id((string)($_GET['id'] ?? ''));
$unlocked = wdf_pin_session_ok($pinFile);
$person = ($unlocked && $id !== null)
    ? wdf_person_fetch($appDir, $configPath, $id) : null;

header("Content-Security-Policy: default-src 'none'; img-src 'self'; style-src 'unsafe-inline'");
header('Cache-Control: no-store');
?>
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Person - WD Face Recognition</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font-family: sans-serif; margin: 1.5em; background: #f6f6f6; }
.grid { display: flex; flex-wrap: wrap; gap: 1em; }
figure { margin: 0; background: #fff; border: 1px solid #ddd;
    border-radius: 6px; padding: .5em; width: 150px; }
figure img { width: 100%; border-radius: 4px; background: #eee; }
figcaption { font-size: .75em; color: #555; word-break: break-all; }
.msg { padding: .6em; border-radius: 4px; background: #fdd; margin-bottom: 1em; }
</style>
</head>
<body>
<p><a href="people.php">Back to people</a></p>
<?php if ($msg !== ''): ?>
<p class="msg"><?= htmlspecialchars($msg, ENT_QUOTES, 'UTF-8') ?></p>
<?php endif; ?>
<?php if (!$unlocked): ?>
<form method="post" action="<?= htmlspecialchars($_SERVER['REQUEST_URI'], ENT_QUOTES, 'UTF-8') ?>">
<label>Demo PIN: <input type="password" name="pin" required></label>
<button type="submit">Unlock</button>
</form>
<?php elseif ($id === null): ?>
<p class="msg">Bad person id.</p>
<?php elseif ($person === null): ?>
<p class="msg">Person not found, or the index database is unreachable.</p>
<?php else: ?>
<h1><?= htmlspecialchars(wdf_person_label($person), ENT_QUOTES, 'UTF-8') ?></h1>
<p><?= count($person['faces']) ?> faces</p>
<div class="grid">
<?php foreach ($person['faces'] as $f) { echo wdf_person_face_html($f); } ?>
</div>
<?php endif; ?>
</body>
</html>
