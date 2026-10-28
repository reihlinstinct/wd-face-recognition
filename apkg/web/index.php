<?php
// WD Face Recognition - status page + on-device demo runner (issue #20).
// The demo runs the packaged wdf_demo binary on a user-chosen folder under
// /shares/ and writes a grouped-faces contact sheet to the reports share.
// No SSH, no cloud: everything executes on this NAS, originals are untouched.
//
// Auth (demo-grade, until the M-wizard lands): the POST requires the
// per-install demo PIN, readable only through authenticated NAS access
// (SMB / the My Cloud web or mobile app), plus a per-session CSRF token.
//
// Runs are ASYNC (issue #33): the POST spawns a detached background job and
// redirects (PRG); the job keeps running when the browser disconnects, and
// the page re-renders live progress from the job's log (meta refresh).
session_start();
require_once __DIR__ . '/paths.php';
require_once __DIR__ . '/run.php';
require_once __DIR__ . '/browse.php';
require_once __DIR__ . '/status.php';
$version = '0.2.3';
$appDir = realpath(dirname(__DIR__));
$demoBin = $appDir . '/bin/wdf_demo';
$modelsDir = $appDir . '/models';
$reportsDir = '/shares/facerecog-reports';
$demoOut = $reportsDir . '/demo-out';
$runLog = $demoOut . '/run.log';
$lockFile = $reportsDir . '/.demo.lock';
$pinFile = $reportsDir . '/demo-pin.txt';
$msg = '';

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

$status = wdf_demo_status_live($runLog);

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $token = (string)($_POST['csrf'] ?? '');
    $pin = trim((string)($_POST['pin'] ?? ''));
    $expectedPin = is_file($pinFile) ? trim((string)file_get_contents($pinFile)) : '';
    $action = (string)($_POST['action'] ?? 'run');

    if (!hash_equals($csrf, $token)) {
        $msg = 'Form token mismatch - reload the page and try again.';
    } elseif ($expectedPin === '' || !hash_equals($expectedPin, $pin)) {
        $msg = 'Wrong demo PIN. Read it from ' . $pinFile . ' over SMB or the My Cloud app. If the share or file is missing, toggle the app off and back on to regenerate both with a fresh PIN.';
    } elseif ($action === 'cancel') {
        // Signal the demo only while the logged PID still belongs to the
        // demo binary (CodeRabbit PR #34): after a crash or NAS reboot the
        // PID may be dead or reused by an unrelated process, and during the
        // startup window (start= written, pid= not yet) there is no PID.
        if ($status['state'] === 'running'
            && $status['pid'] !== null
            && wdf_demo_pid_is_demo($status['pid'])) {
            exec('kill ' . (int)$status['pid'] . ' 2>/dev/null');
        }
        header('Location: ' . strtok($_SERVER['REQUEST_URI'], '?'), true, 303);
        exit;
    } elseif ($status['state'] === 'running') {
        $msg = 'A demo run is already in progress - its progress is shown below.';
    } else {
        $photos = trim((string)($_POST['photos_dir'] ?? ''));
        if (($photosDir = wdf_normalize_photos_dir($photos)) === null) {
            // Accepts shell-escaped spaces ("My\ Folder") and trailing
            // slashes; guards apply to the final resolved path.
            $msg = 'Folder not found on this NAS: ' . htmlspecialchars($photos, ENT_QUOTES, 'UTF-8')
                . ' - paste the path as shown in the file manager, spaces are fine, no backslashes needed.';
        } else {
            if (!is_dir($demoOut)) {
                @mkdir($demoOut, 0777, true);
            }
            // Serialize launches (CodeRabbit PR #34): two concurrent POSTs
            // must not both read an idle status and start overlapping runs.
            $lock = @fopen($lockFile, 'c');
            if (!$lock) {
                // Not contention: the reports share is missing or not
                // writable, so no run can start at all.
                $msg = 'Cannot create the run lock in the facerecog-reports share'
                    . ' - check that the share exists and is writable (toggle the app off and on to recreate it).';
            } elseif (!flock($lock, LOCK_EX | LOCK_NB)) {
                fclose($lock);
                $msg = 'A demo run is already in progress - its progress is shown below.';
            } else {
                // Fresh status re-check under the lock: the other POST may
                // have just launched between our page-read status and here.
                $lockedStatus = wdf_demo_status_live($runLog);
                $ok = false;
                if ($lockedStatus['state'] === 'running') {
                    $msg = 'A demo run is already in progress - its progress is shown below.';
                } else {
                    // Fixed output path: previous demo output is replaced,
                    // never mixed (same guarantee the sync runner had).
                    exec('rm -rf ' . escapeshellarg($demoOut));
                    @mkdir($demoOut, 0777, true);
                    // Async launch (issue #33): detached background job,
                    // output to run.log; survives the browser disconnecting.
                    $ok = wdf_run_demo_async([$demoBin, $photosDir, $modelsDir, $demoOut, '2'], $runLog);
                    if ($ok) {
                        // Hold the lock until the wrapper's start= marker is
                        // visible, so a concurrent POST sees a running state.
                        $deadline = microtime(true) + 3.0;
                        while (microtime(true) < $deadline) {
                            if (wdf_demo_status($runLog)['start'] !== null) {
                                break;
                            }
                            usleep(100000);
                        }
                    }
                }
                flock($lock, LOCK_UN);
                fclose($lock);
                if (!$ok && $msg === '') {
                    $msg = 'Failed to start the demo job - nothing is running. Send this page back with the report.';
                } elseif ($ok) {
                    header('Location: ' . strtok($_SERVER['REQUEST_URI'], '?'), true, 303);
                    exit;
                }
            }
        }
    }
}
header('Content-Type: text/html; charset=utf-8');
$eta = wdf_demo_eta($status, time());
?>
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<?php if ($status['state'] === 'running'): ?>
<meta http-equiv="refresh" content="5">
<?php endif; ?>
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
progress{width:100%;height:1.4em}
.runbox{border:1px solid #ccc;border-radius:6px;padding:10px 12px;margin:12px 0}
.hint{font-size:.9em;color:#444}
</style>
</head>
<body>
<h1>WD Face Recognition</h1>
<p>Version <?= htmlspecialchars($version, ENT_QUOTES, 'UTF-8') ?>.
All processing in this app stays on this NAS.</p>

<?php if ($status['state'] === 'running'): ?>
<div class="runbox">
<h2>Demo running...</h2>
<?php if ($status['total'] !== null): ?>
<progress value="<?= (int)$status['processed'] ?>" max="<?= (int)$status['total'] ?>"></progress>
<p><b><?= (int)$status['processed'] ?> / <?= (int)$status['total'] ?></b> photos processed<?php
    if ($eta !== null): ?> - about <b><?= (int)ceil($eta / 60) ?></b> min left<?php endif; ?>.</p>
<?php else: ?>
<progress></progress>
<p>Counting photos...</p>
<?php endif; ?>
<p class="hint">This page refreshes itself every 5 seconds. The run continues on the NAS
even if you close this tab - come back any time. הריצה ממשיכה גם אם סוגרים את הלשונית.</p>
<form method="post">
<input type="hidden" name="csrf" value="<?= htmlspecialchars($csrf, ENT_QUOTES, 'UTF-8') ?>">
<input type="hidden" name="action" value="cancel">
<label for="pin-cancel">Demo PIN</label> (required to cancel):<br>
<input type="password" id="pin-cancel" name="pin" autocomplete="one-time-code"
       autocapitalize="off" autocorrect="off" spellcheck="false" required>
<button type="submit">Cancel run</button>
</form>
</div>
<?php elseif ($status['state'] === 'done'): ?>
<p class="msg">Demo finished. Open <?= htmlspecialchars($demoOut, ENT_QUOTES, 'UTF-8') ?>/index.html
via SMB or the My Cloud app - it shows your photos grouped by person.</p>
<?php elseif ($status['state'] === 'failed'): ?>
<?php if (!empty($status['orphaned'])): ?>
<p class="msg" style="background:#f2e3e3">Demo stopped unexpectedly - the process is gone and no exit code was recorded
(this can happen after a NAS reboot). You can start a new run. Log tail:</p>
<?php else: ?>
<p class="msg" style="background:#f2e3e3">Demo failed (exit <?= (int)$status['exit'] ?>). Log tail:</p>
<?php endif; ?>
<pre><?= htmlspecialchars(implode("\n", $status['tail']), ENT_QUOTES, 'UTF-8') ?></pre>
<?php endif; ?>

<h2>Demo: group a folder of photos by person</h2>
<form method="post">
<input type="hidden" name="csrf" value="<?= htmlspecialchars($csrf, ENT_QUOTES, 'UTF-8') ?>">
<label for="pin">Demo PIN</label>:<br>
<span class="hint">Open the <b>facerecog-reports</b> share from your computer - the same way you open
your other shares (Windows Explorer / Finder / My Cloud app) - and copy the 12 characters from
<b>\\NAS\facerecog-reports\demo-pin.txt</b>.
If the share or the file is missing, toggle the app off and back on
(Apps &gt; WD Face Recognition) - it recreates both with a fresh PIN.<br>
קוד ה-PIN (12 תווים) כתוב בקובץ demo-pin.txt בתוך השיתוף facerecog-reports -
נכנסים לשיתוף מהמחשב כמו כל שיתוף אחר ומעתיקים.
אם השיתוף או הקובץ חסרים - כבו והדליקו מחדש את האפליקציה וייווצר PIN חדש.</span><br>
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
</body>
</html>
