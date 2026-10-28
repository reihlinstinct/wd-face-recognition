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

if ($fails > 0) {
    fwrite(STDERR, "{$fails} test(s) failed\n");
    exit(1);
}
echo "results_test: all assertions passed\n";
