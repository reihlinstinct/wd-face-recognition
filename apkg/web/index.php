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
require_once __DIR__ . '/auth.php';
wdf_session_start();
require_once __DIR__ . '/paths.php';
require_once __DIR__ . '/run.php';
require_once __DIR__ . '/browse.php';
require_once __DIR__ . '/status.php';
require_once __DIR__ . '/settings.php';
$version = '0.2.5';
$appDir = realpath(dirname(__DIR__));
$demoBin = $appDir . '/bin/wdf_demo';
$modelsDir = $appDir . '/models';
$reportsDir = '/shares/facerecog-reports';
// Configurable output folder (issue #43): read AFTER a possible
// set_output POST below; $demoOut is (re)assigned there.
$demoOut = wdf_output_dir($reportsDir);
$runLog = $demoOut . '/run.log';
$lockFile = $reportsDir . '/.demo.lock';
$pinFile = getenv('WDF_PIN_FILE') !== false ? (string)getenv('WDF_PIN_FILE') : $reportsDir . '/demo-pin.txt';
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

    if ($expectedPin !== '' && hash_equals($expectedPin, $pin)) {
        // Correct PIN (CSRF is still enforced below): remember this
        // browser - 30 days over HTTPS, browser-session over plain HTTP -
        // bound to the current PIN so rotation invalidates it (#35, #42).
        wdf_pin_session_mark($expectedPin);
    }

    if (!hash_equals($csrf, $token)) {
        $msg = 'Form token mismatch - reload the page and try again.';
    } elseif (!wdf_pin_session_ok($pinFile)) {
        $msg = 'Wrong demo PIN. Read it from ' . $pinFile . ' over SMB or the My Cloud app. If the share or file is missing, toggle the app off and back on to regenerate both with a fresh PIN.';
    } elseif ($action === 'set_output') {
        // Configurable output folder (issue #43). Same serialization as a
        // run launch (CodeRabbit PR #44): moving the output folder while a
        // run writes to it would strand the job's log and results.
        $lock = @fopen($lockFile, 'c');
        if (!$lock) {
            $msg = 'Cannot create the run lock in the facerecog-reports share'
                . ' - check that the share exists and is writable.';
        } elseif (!flock($lock, LOCK_EX | LOCK_NB)) {
            fclose($lock);
            $msg = 'A demo run is in progress - change the output folder when it finishes.';
        } else {
            if (wdf_demo_status_live($runLog)['state'] === 'running') {
                $msg = 'A demo run is in progress - change the output folder when it finishes.';
            } else {
                $setErr = wdf_set_output_dir((string)($_POST['output_dir'] ?? ''), $reportsDir);
                $msg = $setErr === ''
                    ? 'Output folder updated - the existing results were moved there.'
                    : $setErr;
                $demoOut = wdf_output_dir($reportsDir);
                $runLog = $demoOut . '/run.log';
                $status = wdf_demo_status_live($runLog);
            }
            flock($lock, LOCK_UN);
            fclose($lock);
        }
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
        } elseif (wdf_paths_overlap(wdf_realpath_lenient($photosDir), wdf_realpath_lenient($demoOut))) {
            // Both directions (CodeRabbit PR #34): a source inside demo-out
            // is deleted by the pre-run cleanup; a source CONTAINING
            // demo-out (e.g. the reports share itself) makes the demo scan
            // its own output mid-run and puts the cleanup inside the
            // chosen tree. Resolved paths: "/shares/x/./demo-out" slips
            // past a raw prefix compare.
            $msg = 'That folder overlaps the demo output folder (' . $demoOut . '),'
                . ' which is wiped and rewritten on each run - choose a different photo folder.';
        } else {
            // Legacy (pre-0.2.5) default output has no marker; backfill it
            // so the replaceable check below recognizes app content.
            if ($demoOut === wdf_default_output($reportsDir)
                && is_file($demoOut . '/index.html')) {
                wdf_mark_output_dir($demoOut);
            }
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
                } elseif (!wdf_output_dir_replaceable($demoOut)) {
                    // The configured folder holds files this app did not
                    // create: never wipe them (issue #43).
                    $msg = 'The output folder is not empty and was not created by this app'
                        . ' - refusing to replace it. Pick another output folder below'
                        . ' or empty it yourself.';
                } else {
                    // Fixed output path: previous demo output is replaced,
                    // never mixed (same guarantee the sync runner had). A
                    // failed cleanup must abort the launch: a leftover
                    // run.log could feed a previous run's exit marker to
                    // the status parser (CodeRabbit PR #34).
                    exec('rm -rf ' . escapeshellarg($demoOut) . ' 2>/dev/null', $rmLines, $rmRc);
                    if ($rmRc !== 0 || !@mkdir($demoOut, 0777, true)) {
                        $msg = 'Could not reset the demo output folder - nothing was started.'
                            . ' Check that the output share is writable.';
                    } else {
                        wdf_mark_output_dir($demoOut);
                        // Async launch (issue #33): detached background job,
                        // output to run.log; survives the browser disconnecting.
                        $ok = wdf_run_demo_async([$demoBin, $photosDir, $modelsDir, $demoOut, '2'], $runLog);
                        if ($ok) {
                            // Hold the lock until the wrapper's start= marker
                            // is visible, so a concurrent POST sees running.
                            $deadline = microtime(true) + 3.0;
                            while (microtime(true) < $deadline) {
                                if (wdf_demo_status($runLog)['start'] !== null) {
                                    break;
                                }
                                usleep(100000);
                            }
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
<?php if (!wdf_pin_session_ok($pinFile)): ?>
<label for="pin-cancel">Demo PIN</label> (required to cancel):<br>
<input type="password" id="pin-cancel" name="pin" autocomplete="one-time-code"
       autocapitalize="off" autocorrect="off" spellcheck="false" required>
<?php endif; ?>
<button type="submit">Cancel run</button>
</form>
</div>
<?php elseif ($status['state'] === 'done'): ?>
<p class="msg">Demo finished. <a href="results.php"><b>Open the results in this browser</b></a>
(after your demo PIN) - or open <?= htmlspecialchars($demoOut, ENT_QUOTES, 'UTF-8') ?>/index.html
via SMB or the My Cloud app. Your photos grouped by person. התוצאות נפתחות גם בדפדפן דרך הקישור למעלה.</p>
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
<?php if (!wdf_pin_session_ok($pinFile)): ?>
<label for="pin">Demo PIN</label> (first time on this browser - remembered 30 days over HTTPS, until you close the browser over plain HTTP):<br>
<span class="hint">Open the <b>facerecog-reports</b> share from your computer - the same way you open
your other shares (Windows Explorer / Finder / My Cloud app) - and copy the 12 characters from
<b>\\NAS\facerecog-reports\demo-pin.txt</b>.
If the share or the file is missing, toggle the app off and back on
(Apps &gt; WD Face Recognition) - it recreates both with a fresh PIN.<br>
קוד ה-PIN (12 תווים) כתוב בקובץ demo-pin.txt בתוך השיתוף facerecog-reports -
נכנסים לשיתוף מהמחשב כמו כל שיתוף אחר ומעתיקים. מסמנים פעם אחת - ב-HTTPS נזכר 30 יום, אחרת עד סגירת הדפדפן.
אם השיתוף או הקובץ חסרים - כבו והדליקו מחדש את האפליקציה וייווצר PIN חדש.</span><br>
<input type="password" id="pin" name="pin" autocomplete="one-time-code"
       autocapitalize="off" autocorrect="off" spellcheck="false" required><br><br>
<?php endif; ?>
<label for="photos_dir">Photo folder</label> (absolute path under /shares/; subfolders are scanned too, up to 8 levels and 1000 photos per run; spaces and Hebrew names are fine - paste as-is, no backslashes needed):<br>
<input type="text" id="photos_dir" name="photos_dir"
       placeholder="/shares/Public/demo-photos"
       value="<?= htmlspecialchars($prefillDir, ENT_QUOTES, 'UTF-8') ?>" required><br>
<a href="?browse=/shares">Browse folders on this NAS</a><br><br>
<button type="submit">Run demo</button>
</form>
<?= $browseHtml ?>

<h2>Output folder</h2>
<form method="post">
<input type="hidden" name="csrf" value="<?= htmlspecialchars($csrf, ENT_QUOTES, 'UTF-8') ?>">
<input type="hidden" name="action" value="set_output">
<?php if (!wdf_pin_session_ok($pinFile)): ?>
<label for="pin-output">Demo PIN</label> (required to change settings):<br>
<input type="password" id="pin-output" name="pin" autocomplete="one-time-code"
       autocapitalize="off" autocorrect="off" spellcheck="false" required><br>
<?php endif; ?>
<label for="output_dir">Results folder</label> - current: <b><?= htmlspecialchars($demoOut, ENT_QUOTES, 'UTF-8') ?></b><br>
<span class="hint">A subfolder inside any existing share (not a share itself). Changing it MOVES the
existing results there. Every run replaces the folder's contents, and the app refuses to touch a
folder it did not create. תיקיית התוצאות - אפשר לבחור תיקייה בתוך כל שיתוף קיים, והתוצאות הקיימות מועברות אליה.</span><br>
<input type="text" id="output_dir" name="output_dir"
       placeholder="/shares/facerecog-reports/demo-out" required><br>
<button type="submit">Change output folder</button>
</form>

<?php if ($msg !== ''): ?>
<p class="msg"><?= $msg ?></p>
<?php endif; ?>
</body>
</html>
