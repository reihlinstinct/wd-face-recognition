<?php
// People page (issue #50, 0.3.0 PR 3d): lists the clusters from the face
// index with rename and merge actions. The stripped OS5 PHP has no sqlite
// extension, so the page shells out to wdf_faces for JSON and mutations.
require_once __DIR__ . '/auth.php';
wdf_session_start();
require_once __DIR__ . '/paths.php';
require_once __DIR__ . '/people_lib.php';

// Tests point the page at a stub app dir (issue #95), like the
// WDF_PIN_FILE / WDF_WFI_CONFIG overrides.
$appOverride = getenv('WDF_APP_DIR');
$appDir = ($appOverride !== false && $appOverride !== '')
    ? $appOverride : realpath(dirname(__DIR__));
$configPath = wdf_wfi_config_path($appDir);

$reportsDir = '/shares/facerecog-reports';
$pinOverride = getenv('WDF_PIN_FILE');
$pinFile = ($pinOverride !== false && $pinOverride !== '')
    ? $pinOverride : $reportsDir . '/demo-pin.txt';
$expectedPin = is_readable($pinFile)
    ? trim((string)file_get_contents($pinFile)) : '';

if (empty($_SESSION['csrf'])) {
    $_SESSION['csrf'] = bin2hex(random_bytes(16));
}
$csrf = $_SESSION['csrf'];

$msg = '';
$msgOk = false;

// Mutations: PIN (or its remembered-session marker) plus the per-session
// CSRF token, then PRG so a refresh never repeats the write.
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $pin = (string)($_POST['pin'] ?? '');
    $token = (string)($_POST['csrf'] ?? '');
    if ($expectedPin !== '' && hash_equals($expectedPin, $pin)) {
        wdf_pin_session_mark($expectedPin);
    }
    if (!hash_equals($csrf, $token)) {
        $msg = 'Form token mismatch - reload the page and try again.';
    } elseif (!wdf_pin_session_ok($pinFile)) {
        $msg = 'Wrong demo PIN. Read it from ' . $pinFile
            . ' over SMB or the My Cloud app.';
    } else {
        $action = (string)($_POST['action'] ?? '');
        if ($action === 'rename') {
            $id = wdf_post_id((string)($_POST['id'] ?? ''));
            $name = wdf_name_trim((string)($_POST['name'] ?? ''));
            $renameErr = wdf_rename_request_error((string)($_POST['id'] ?? ''),
                (string)($_POST['name'] ?? ''));
            if ($renameErr !== '') {
                $msg = $renameErr;
            } else {
                [$rc, $out] = wdf_faces_call($appDir,
                    ['rename', '--config', $configPath,
                     '--id', (string)$id, '--name', $name]);
                if ($rc === 0) {
                    header('Location: people.php?ok=renamed', true, 303);
                    exit;
                }
                $msg = 'Rename failed - is the index database reachable?';
            }
        } elseif ($action === 'merge') {
            $from = wdf_post_id((string)($_POST['from'] ?? ''));
            $to = wdf_post_id((string)($_POST['to'] ?? ''));
            $mergeErr = wdf_merge_request_error((string)($_POST['from'] ?? ''),
                (string)($_POST['to'] ?? ''), !empty($_POST['confirm']));
            if ($mergeErr !== '') {
                $msg = $mergeErr;
            } else {
                [$rc, $out] = wdf_faces_call($appDir,
                    ['merge', '--config', $configPath,
                     '--from', (string)$from, '--to', (string)$to]);
                if ($rc === 0) {
                    header('Location: people.php?ok=merged', true, 303);
                    exit;
                }
                $msg = 'Merge failed - is the index database reachable?';
            }
        } else {
            $msg = 'Unknown action.';
        }
    }
}

if (isset($_GET['ok'])) {
    $msg = $_GET['ok'] === 'merged' ? 'People merged.' : 'Name saved.';
    $msgOk = true;
}

$people = wdf_pin_session_ok($pinFile)
    ? wdf_people_fetch($appDir, $configPath) : null;
$unlocked = wdf_pin_session_ok($pinFile);

header("Content-Security-Policy: default-src 'none'; img-src 'self'; style-src 'unsafe-inline'");
header('Cache-Control: no-store');
?>
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>People - WD Face Recognition</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font-family: sans-serif; margin: 1.5em; background: #f6f6f6; }
.grid { display: flex; flex-wrap: wrap; gap: 1em; }
a.card { display: block; background: #fff; border: 1px solid #ddd;
    border-radius: 6px; padding: .6em; text-decoration: none;
    color: #222; width: 150px; }
a.card img { width: 100%; border-radius: 4px; background: #eee; }
a.card .name { display: block; font-weight: bold; margin-top: .4em;
    word-break: break-word; }
a.card .count { display: block; color: #666; font-size: .85em; }
.msg { padding: .6em; border-radius: 4px; background: #fdd; margin-bottom: 1em; }
.msg.ok { background: #dfd; }
form.inline { display: inline; }
input[type=text] { padding: .3em; }
select { padding: .3em; }
button { padding: .35em .8em; }
section { background: #fff; border: 1px solid #ddd; border-radius: 6px;
    padding: 1em; margin-top: 1.5em; max-width: 34em; }
</style>
</head>
<body>
<h1>People</h1>
<p><a href="index.php">Back to dashboard</a></p>
<?php if ($msg !== ''): ?>
<p class="msg<?= $msgOk ? ' ok' : '' ?>"><?= htmlspecialchars($msg, ENT_QUOTES, 'UTF-8') ?></p>
<?php endif; ?>
<?php if (!$unlocked): ?>
<form method="post" action="people.php">
<input type="hidden" name="csrf" value="<?= htmlspecialchars($csrf, ENT_QUOTES, 'UTF-8') ?>">
<label>Demo PIN: <input type="password" name="pin" required></label>
<button type="submit">Unlock</button>
</form>
<?php elseif ($people === null): ?>
<p class="msg">Could not read the face index. Run an indexing pass first.</p>
<?php elseif (count($people) === 0): ?>
<p>No people found yet. Run an indexing pass first.</p>
<?php else: ?>
<div class="grid">
<?php foreach ($people as $p) { echo wdf_person_card_html($p); } ?>
</div>
<section>
<h2>Rename</h2>
<form method="post" action="people.php">
<input type="hidden" name="csrf" value="<?= htmlspecialchars($csrf, ENT_QUOTES, 'UTF-8') ?>">
<input type="hidden" name="action" value="rename">
<label>Person: <select name="id">
<?php foreach ($people as $p): ?>
<option value="<?= (int)$p['id'] ?>"><?= htmlspecialchars(wdf_person_label($p), ENT_QUOTES, 'UTF-8') ?></option>
<?php endforeach; ?>
</select></label>
<label>New name: <input type="text" name="name" maxlength="128" required></label>
<button type="submit">Save name</button>
</form>
</section>
<section>
<h2>Merge two people</h2>
<p>Every face of the first person moves to the second. This cannot be undone.</p>
<form method="post" action="people.php">
<input type="hidden" name="csrf" value="<?= htmlspecialchars($csrf, ENT_QUOTES, 'UTF-8') ?>">
<input type="hidden" name="action" value="merge">
<label>Merge: <select name="from">
<?php foreach ($people as $p): ?>
<option value="<?= (int)$p['id'] ?>"><?= htmlspecialchars(wdf_person_label($p), ENT_QUOTES, 'UTF-8') ?></option>
<?php endforeach; ?>
</select></label>
<label>into: <select name="to">
<?php foreach ($people as $p): ?>
<option value="<?= (int)$p['id'] ?>"><?= htmlspecialchars(wdf_person_label($p), ENT_QUOTES, 'UTF-8') ?></option>
<?php endforeach; ?>
</select></label>
<label><input type="checkbox" name="confirm" value="1" required>
I understand a merge cannot be undone.</label>
<button type="submit">Merge</button>
</form>
</section>
<?php endif; ?>
</body>
</html>
