<?php
// Tests for the in-browser results endpoint (issue #35): the crop image
// guard must reject traversal, subdirectories, non-jpeg names and missing
// files, and must resolve symlink tricks back inside the crops directory.
// Run: php apkg/web/tests/results_test.php
require_once __DIR__ . '/../paths.php';

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

$tmp = sys_get_temp_dir() . '/wdf-results-' . getmypid();
$crops = $tmp . '/demo-out/crops';
@mkdir($crops, 0777, true);
file_put_contents($crops . '/f1.jpg', 'jpeg-bytes');
file_put_contents($tmp . '/demo-out/secret.jpg', 'outside');
@mkdir($tmp . '/outside', 0777, true);
file_put_contents($tmp . '/outside/evil.jpg', 'evil');
@symlink($tmp . '/outside/evil.jpg', $crops . '/link.jpg');

// Happy path.
check(wdf_safe_crop_path($crops, 'f1.jpg') === realpath($crops . '/f1.jpg'),
      'a real crop resolves');

// Rejections.
check(wdf_safe_crop_path($crops, '../secret.jpg') === null, 'dot-dot traversal rejected');
check(wdf_safe_crop_path($crops, '../../etc/passwd') === null, 'path traversal rejected');
check(wdf_safe_crop_path($crops, 'sub/f1.jpg') === null, 'subdirectory rejected');
check(wdf_safe_crop_path($crops, 'f1.png') === null, 'non-jpeg rejected');
check(wdf_safe_crop_path($crops, 'f1.jpg/') === null, 'trailing slash rejected');
check(wdf_safe_crop_path($crops, '') === null, 'empty name rejected');
check(wdf_safe_crop_path($crops, 'nope.jpg') === null, 'missing file rejected');
check(wdf_safe_crop_path($crops, 'link.jpg') === null,
      'symlink escaping the crops dir rejected');
check(wdf_safe_crop_path($tmp . '/no-such-dir', 'f1.jpg') === null,
      'missing crops dir rejected');

// The index.html rewrite sends every relative crop URL through results.php.
$sample = "<h2>Person 1</h2><div class=grid><img src='crops/f1.jpg' title='a.jpg'>"
    . "<img src='crops/f2.jpg' title='b.jpg'></div>";
$rewritten = preg_replace("#src='crops/([^']+)'#", "src='results.php?img=\$1'", $sample);
check(strpos($rewritten, "src='results.php?img=f1.jpg'") !== false
      && strpos($rewritten, "src='results.php?img=f2.jpg'") !== false,
      'crop URLs rewrite to the endpoint');
check(strpos($rewritten, "src='crops/") === false, 'no direct crop URLs remain');

// Live harness (CodeRabbit PR #44): serve the REAL results.php through
// the built-in web server and assert the exact emitted headers - a
// source grep cannot prove what a response actually carries.
$live = sys_get_temp_dir() . '/wdf-results-live-' . getmypid();
@mkdir($live . '/out/crops', 0777, true);
@mkdir($live . '/sess', 0777, true);
file_put_contents($live . '/out/index.html', '<h1>r</h1>');
file_put_contents($live . '/out/crops/f1.jpg', 'jpeg');
file_put_contents($live . '/state.json', json_encode(['output_dir' => $live . '/out']));
file_put_contents($live . '/pin.txt', "0123abcd4567\n");
$port = 20000 + (getmypid() % 20000);
$serverCmd = sprintf(
    'WDF_STATE_FILE=%s WDF_PIN_FILE=%s %s -d display_errors=0 -d session.save_path=%s -S 127.0.0.1:%d -t %s >/dev/null 2>&1 & echo $!',
    escapeshellarg($live . '/state.json'), escapeshellarg($live . '/pin.txt'),
    escapeshellarg(PHP_BINARY), escapeshellarg($live . '/sess'), $port,
    escapeshellarg((string)realpath(__DIR__ . '/..')));
$serverPid = (int)shell_exec($serverCmd);
$base = 'http://127.0.0.1:' . $port;
$up = false;
for ($i = 0; $i < 40; $i++) {
    $c = @file_get_contents($base . '/results.php');
    if ($c !== false) { $up = true; break; }
    usleep(100000);
}
check($up, 'test server started');

function wdf_live_headers(string $url): array
{
    $ctx = stream_context_create(['http' => ['method' => 'GET', 'ignore_errors' => true]]);
    @file_get_contents($url, false, $ctx);
    return $http_response_header ?? [];
}

if ($up) {
    $h = wdf_live_headers($base . '/results.php');
    check(in_array("Content-Security-Policy: default-src 'none'; img-src 'self'; style-src 'unsafe-inline'", $h, true),
          'emitted CSP header is exact');
    check(in_array('Cache-Control: no-store', $h, true), 'emitted Cache-Control is no-store');
    $h = wdf_live_headers($base . '/results.php?img=f1.jpg');
    check(isset($h[0]) && preg_match('#^HTTP/\S+ 403#', (string)$h[0]) === 1,
          'crop request without a session is 403');
}
if ($serverPid > 0) {
    shell_exec('kill ' . $serverPid . ' 2>/dev/null');
}

if ($fails > 0) {
    fwrite(STDERR, "{$fails} test(s) failed\n");
    exit(1);
}
echo "results_test: all assertions passed\n";
