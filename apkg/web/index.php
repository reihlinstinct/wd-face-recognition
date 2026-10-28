<?php
// WD Face Recognition - status page + on-device demo runner (issue #20).
// The demo runs the packaged wdf_demo binary on a user-chosen folder under
// /shares/ and writes a grouped-faces contact sheet to the reports share.
// No SSH, no cloud: everything executes on this NAS, originals are untouched.
//
// Auth (demo-grade, until the M-wizard lands): the POST requires the
// per-install demo PIN, readable only through the authenticated WD dashboard
// or SMB (/shares/facerecog-reports/demo-pin.txt), plus a per-session CSRF
// token. Runs are serialized with an exclusive lock.
session_start();
require_once __DIR__ . '/paths.php';
require_once __DIR__ . '/run.php';
require_once __DIR__ . '/browse.php';
$version = '0.2.2';
$appDir = realpath(dirname(__DIR__));
$demoBin = $appDir . '/bin/wdf_demo';
$modelsDir = $appDir . '/models';
$reportsDir = '/shares/facerecog-reports';
$demoOut = $reportsDir . '/demo-out';
$pinFile = $reportsDir . '/demo-pin.txt';
$lockFile = $reportsDir . '/.demo.lock';
$msg = '';
$log = '';

// Folder browser (issue #26): read-only GET navigation under /shares.
// "Use this folder" prefills the form; the POST still validates the path.
$browseHtml = '';
$prefillDir = '';
if (isset($_GET['browse'])) {
    $listing = wdf_browse_list((string)$_GET['browse']);
    $browseHtml = $listing === null
        ? '<p class="msg">Folder not found or not allowed under /shares.</p>'
        : wdf_browse_render($listing);
} elseif (isset($_GET['select'])) {
    $prefillDir = (string)$_GET['select'];
    $listing = wdf_browse_list($prefillDir);
    if ($listing !== null) {
        $browseHtml = wdf_browse_render($listing);
    }
}

if (empty($_SESSION['csrf'])) {
    $_SESSION['csrf'] = bin2hex(random_bytes(16));
}
$csrf = $_SESSION['csrf'];

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $token = (string)($_POST['csrf'] ?? '');
    $pin = trim((string)($_POST['pin'] ?? ''));
    $expectedPin = is_file($pinFile) ? trim((string)file_get_contents($pinFile)) : '';
    $photos = trim((string)($_POST['photos_dir'] ?? ''));

    if (!hash_equals($csrf, $token)) {
        $msg = 'Form token mismatch - reload the page and try again.';
    } elseif ($expectedPin === '' || !hash_equals($expectedPin, $pin)) {
        $msg = 'Wrong demo PIN. Read it from ' . $pinFile
            . ' via the WD dashboard file manager or SMB.';
    } elseif (($photosDir = wdf_normalize_photos_dir($photos)) === null) {
        // Accepts shell-escaped spaces ("My\ Folder") and trailing slashes;
        // guards (under /shares/, no "..") apply to the final resolved path.
        $msg = 'Folder not found on this NAS: ' . htmlspecialchars($photos, ENT_QUOTES, 'UTF-8')
            . ' - paste the path as shown in the file manager, spaces are fine, no backslashes needed.';
    } else {
        if (!is_dir($reportsDir)) {
            @mkdir($reportsDir, 0777, true);
        }
        $lock = fopen($lockFile, 'c');
        if (!$lock || !flock($lock, LOCK_EX | LOCK_NB)) {
            $msg = 'A demo run is already in progress - wait for it to finish.';
        } else {
            // Fixed output path: previous demo output is replaced, never mixed.
            exec('rm -rf ' . escapeshellarg($demoOut));
            // No shell: argv array preserves non-ASCII (Hebrew) and spaced
            // paths byte-for-byte under any LC_CTYPE (see run.php).
            [$rc, $lines] = wdf_run_demo([$demoBin, $photosDir, $modelsDir, $demoOut, '2']);
            flock($lock, LOCK_UN);
            fclose($lock);
            $log = htmlspecialchars(implode("\n", (array)$lines), ENT_QUOTES, 'UTF-8');
            if ($rc === 0 && is_file($demoOut . '/index.html')) {
                $msg = 'Demo finished. Open ' . $demoOut . '/index.html via the WD dashboard '
                    . 'file manager or SMB - it shows your photos grouped by person.';
            } else {
                $msg = 'Demo failed (exit ' . (int)$rc . '). Send the log below back with the report.';
            }
        }
    }
}
header('Content-Type: text/html; charset=utf-8');
?>
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WD Face Recognition</title>
<style>
body{font-family:system-ui;margin:16px;max-width:40em}
input[type=text],input[type=password]{width:100%;padding:8px;font-size:1em;box-sizing:border-box}
button{padding:10px 18px;font-size:1em}
pre{background:#eee;padding:8px;overflow:auto;font-size:.85em}
.browse ul{list-style:none;padding-left:8px;line-height:1.9}
.browse{border:1px solid #ccc;border-radius:6px;padding:8px 12px;margin:12px 0}
.msg{padding:10px;background:#e8f2e8;border-radius:6px}
label{font-weight:600}
</style>
</head>
<body>
<h1>WD Face Recognition</h1>
<p>Version <?= htmlspecialchars($version, ENT_QUOTES, 'UTF-8') ?>.
All processing in this app stays on this NAS.</p>
<h2>Demo: group a folder of photos by person</h2>
<form method="post">
<input type="hidden" name="csrf" value="<?= htmlspecialchars($csrf, ENT_QUOTES, 'UTF-8') ?>">
<label for="pin">Demo PIN</label> (read it from <?= htmlspecialchars($pinFile, ENT_QUOTES, 'UTF-8') ?>
via the WD dashboard file manager or SMB):<br>
<input type="password" id="pin" name="pin" autocomplete="one-time-code"
       autocapitalize="off" autocorrect="off" spellcheck="false" required><br><br>
<label for="photos_dir">Photo folder</label> (absolute path under /shares/; subfolders are scanned too, up to 8 levels and 1000 photos per run; spaces and Hebrew names are fine - paste as-is, no backslashes needed):<br>
<input type="text" id="photos_dir" name="photos_dir"
       placeholder="/shares/Public/demo-photos"
       value="<?= htmlspecialchars($prefillDir, ENT_QUOTES, 'UTF-8') ?>" required><br>
<a href="?browse=/shares">Browse folders on this NAS</a><br><br>
<button type="submit">Run demo</button>
</form>
<?= $browseHtml ?>
<?php if ($msg !== ''): ?>
<p class="msg"><?= $msg ?></p>
<?php endif; ?>
<?php if ($log !== ''): ?>
<h3>Run log</h3>
<pre><?= $log ?></pre>
<?php endif; ?>
</body>
</html>
