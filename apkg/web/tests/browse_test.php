<?php
// Tests for the demo folder browser (issue #26).
// Run: php apkg/web/tests/browse_test.php
require_once __DIR__ . '/../browse.php';

$tmp = sys_get_temp_dir() . '/wdf-browse-' . getmypid();
$prefix = $tmp . '/shares/';
@mkdir($tmp . '/shares/Photos/sub/deep', 0777, true);
@mkdir($tmp . '/shares/Photos/with space', 0777, true);
@mkdir($tmp . '/shares/Photos/תמונות', 0777, true);
@mkdir($tmp . '/shares/Photos/<b>&amp;', 0777, true);
@mkdir($tmp . '/shares/Photos/.hidden', 0777, true);
file_put_contents($tmp . '/shares/Photos/a.jpg', 'x');
file_put_contents($tmp . '/shares/Photos/b.JPEG', 'x');
file_put_contents($tmp . '/shares/Photos/sub/c.jpg', 'x');
file_put_contents($tmp . '/shares/Photos/sub/deep/d.jpg', 'x');
file_put_contents($tmp . '/shares/Photos/with space/e.jpg', 'x');
file_put_contents($tmp . '/shares/Photos/תמונות/f.jpg', 'x');
file_put_contents($tmp . '/shares/Photos/notes.txt', 'x');
file_put_contents($tmp . '/shares/Photos/.jpg', 'x'); // empty basename: not a photo
@symlink('/etc', $tmp . '/shares/Photos/linkdir');
@symlink($tmp . '/shares/Photos/a.jpg', $tmp . '/shares/Photos/link.jpg');

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

$photos = $prefix . 'Photos';
$listing = wdf_browse_list($photos, $prefix);
check($listing !== null, 'lists photos dir');
check($listing['dir'] === $photos, 'normalized dir');
check($listing['jpeg_count'] === 6, 'recursive jpeg count (symlink file and .jpg basename excluded)');
$names = array_column($listing['subdirs'], 'name');
sort($names);
check($names === ['<b>&amp;', 'sub', 'with space', 'תמונות'], 'subdir names: spaces/Hebrew/HTML chars in, hidden/symlink/.. out');
$counts = array_column($listing['subdirs'], 'jpeg_count', 'name');
check($counts['sub'] === 2, 'nested count');
check($counts['with space'] === 1, 'count in spaced name');

// Browsing the shares root itself works.
$root = wdf_browse_list($prefix, $prefix);
check($root !== null && $root['dir'] === rtrim($prefix, '/'), 'root browsable');
check($root !== null && array_column($root['subdirs'], 'name') === ['Photos'], 'root lists Photos');

// Guards.
check(wdf_browse_list('/etc', $prefix) === null, 'outside prefix rejected');
check(wdf_browse_list($prefix . '../shares/Photos', $prefix) === null, 'dotdot rejected');
check(wdf_browse_list($tmp . '/shares/missing', $prefix) === null, 'missing rejected');
check(wdf_browse_list($tmp . '/shares/Photos/linkdir', $prefix) === null, 'symlink dir rejected');

// Count caps.
check(wdf_browse_count_jpegs($photos, 3) === 3, 'count capped at maxPhotos');
check(wdf_browse_count_jpegs($photos, 1000, 1) === 2, 'depth cap 1 = top-level files only');
check(wdf_browse_count_jpegs($tmp . '/shares/missing') === -1, 'unreadable root = -1');

// Render: escaped output, links, up-navigation.
$html = wdf_browse_render($listing, $prefix);
check(strpos($html, '&lt;b&gt;&amp;amp;') !== false, 'HTML-special folder name escaped');
check(strpos($html, '<b>&amp;/</a>') === false, 'raw HTML name absent');
check(strpos($html, urlencode('with space')) !== false, 'browse link urlencoded');
check(strpos($html, 'Use this folder') !== false, 'select link present');
check(strpos($html, '?select=' . urlencode($photos)) !== false, 'select link targets current dir');
check(strpos($html, '.. (up)') !== false, 'up link in subdir');
check(strpos($html, '6 JPEGs in this folder tree') !== false, 'current tree count shown');
$rootHtml = wdf_browse_render($root, $prefix);
check(strpos($rootHtml, '.. (up)') === false, 'no up link at shares root');

if ($fails > 0) {
    fwrite(STDERR, "{$fails} test(s) failed\n");
    exit(1);
}
echo "browse_test: all assertions passed\n";
