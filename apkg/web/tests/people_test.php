<?php
// Tests for the people pages backend (issue #50): config parsing, JSON
// shape validation, the label fallback, HTML escaping, post-id parsing,
// and the exec+parse path against a stub wdf_faces. Run:
// php apkg/web/tests/people_test.php
require_once __DIR__ . '/../people_lib.php';

$fails = 0;
function check(bool $cond, string $label): void {
    global $fails;
    if (!$cond) {
        $fails++;
        fwrite(STDERR, "FAIL: {$label}\n");
    }
}

$tmp = sys_get_temp_dir() . '/wdf-people-' . getmypid();
@mkdir($tmp, 0777, true);

// --- config parsing -----------------------------------------------------
file_put_contents($tmp . '/wfi.conf',
    "# comment line\n"
    . "roots=/shares/Photos,/shares/More\n"
    . "data_dir = /shares/Apps/wd-face-recognition/data\n"
    . "threshold=0.5\n"
    . "\n"
    . "stray line without equals\n");
$cfg = wdf_wfi_config_load($tmp . '/wfi.conf');
check(is_array($cfg), 'config parses');
check($cfg['data_dir'] === '/shares/Apps/wd-face-recognition/data',
    'data_dir parsed, whitespace trimmed');
check($cfg['roots'] === '/shares/Photos,/shares/More', 'roots parsed');
check(wdf_wfi_config_load($tmp . '/missing.conf') === null,
    'missing config returns null');

putenv('WDF_WFI_CONFIG=' . $tmp . '/wfi.conf');
check(wdf_wfi_config_path('/app') === $tmp . '/wfi.conf',
    'WDF_WFI_CONFIG env override wins');
putenv('WDF_WFI_CONFIG');
check(wdf_wfi_config_path('/app') === '/app/wfi.conf',
    'default config path is next to the app');

// --- people JSON validation ---------------------------------------------
check(wdf_people_valid([]), 'empty people list is valid');
check(wdf_people_valid([['id' => 1, 'name' => '', 'faces' => 3,
    'cover' => 'crops/p1_t1_f0.jpg']]), 'one valid person');
check(wdf_people_valid([['id' => '1', 'name' => '', 'faces' => 3,
    'cover' => 'crops/x.jpg']]) === false, 'string id rejected');
check(wdf_people_valid([['id' => 1, 'name' => '', 'faces' => 3]]) === false,
    'missing cover rejected');
check(wdf_people_valid(['id' => 1]) === false, 'assoc shape rejected');
check(wdf_people_valid(null) === false, 'non-array rejected');

check(wdf_person_valid(['id' => 1, 'name' => 'Dana', 'faces' => [
    ['id' => 5, 'bbox' => '1,2,3,4', 'crop' => 'crops/a.jpg',
     'photo' => '/shares/Photos/a.jpg'],
]]), 'valid person');
check(wdf_person_valid(['id' => 1, 'name' => 'Dana', 'faces' => [
    ['id' => 5, 'bbox' => '1,2,3,4', 'crop' => 'crops/a.jpg'],
]]) === false, 'face missing photo rejected');

// --- label fallback + escaping ------------------------------------------
check(wdf_person_label(['id' => 7, 'name' => '']) === 'Person 7',
    'unnamed cluster gets Person N');
check(wdf_person_label(['id' => 7, 'name' => 'דנה']) === 'דנה',
    'Hebrew name passes through');
$html = wdf_person_card_html(['id' => 2, 'name' => '<b>x</b>', 'faces' => 4,
    'cover' => 'crops/p2_t1_f0.jpg']);
check(strpos($html, '<b>x</b>') === false, 'hostile name not raw');
check(strpos($html, '&lt;b&gt;') !== false, 'name is escaped');
check(strpos($html, 'person.php?id=2') !== false, 'card links to person page');
check(strpos($html, 'crop.php?f=p2_t1_f0.jpg') !== false,
    'cover passed as basename');
$faceHtml = wdf_person_face_html(['id' => 9, 'bbox' => '0,0,1,1',
    'crop' => 'crops/p9_t1_f0.jpg',
    'photo' => '/shares/Photos/<script>.jpg']);
check(strpos($faceHtml, '<script>') === false, 'photo path not rendered');
check(strpos($faceHtml, 'Photos') === false,
    'source photo path never reaches the person page (review #58)');

// --- post id parsing ------------------------------------------------------
check(wdf_post_id('42') === 42, 'digits parse');
check(wdf_post_id('0') === null, 'zero rejected');
check(wdf_post_id('-3') === null, 'negative rejected');
check(wdf_post_id('1 OR 1=1') === null, 'injection rejected');
check(wdf_post_id('') === null, 'empty rejected');

// --- merge request gate (review #58) ---------------------------------------
check(wdf_merge_request_error('1', '1', true) !== '', 'self-merge refused');
check(wdf_merge_request_error('x', '2', true) !== '', 'bad from id refused');
check(wdf_merge_request_error('1', '2', false) !== '',
    'merge without confirmation refused');
check(wdf_merge_request_error('1', '2', true) === '',
    'confirmed merge of two people passes');

// --- rename request gate (issue #84) --------------------------------------
check(wdf_rename_request_error('x', 'Dana') !== '', 'bad rename id refused');
check(wdf_rename_request_error('1', '') !== '', 'empty rename name refused');
check(wdf_rename_request_error('1', '   ') !== '',
    'whitespace-only name refused');
check(strpos(wdf_rename_request_error('1', ''), 'name') !== false,
    'empty-name message is specific');
check(wdf_rename_request_error('1', str_repeat('a', 129)) !== '',
    'over-long name refused');
check(wdf_rename_request_error('1', str_repeat('a', 128)) === '',
    '128-char name passes');
check(wdf_rename_request_error('1', '  דנה  ') === '',
    'Hebrew name with padding passes');
check(wdf_rename_request_error('1', 'Dana') === '', 'valid rename passes');
// The cap counts characters, not bytes (review #85). mbstring is
// REQUIRED for these assertions - fail loudly if the CI php lacks it
// rather than silently skipping the character-cap pin.
check(function_exists('mb_strlen'), 'mbstring available for cap tests');
if (function_exists('mb_strlen')) {
    check(wdf_rename_request_error('1', str_repeat('ד', 100)) === '',
        '100 Hebrew characters accepted');
    check(wdf_rename_request_error('1', str_repeat('ד', 129)) !== '',
        '129 Hebrew characters refused');
    check(strpos(wdf_rename_request_error('1', str_repeat('a', 129)),
        'characters') !== false, 'mbstring message says characters');
}

// --- exec + parse through a stub wdf_faces --------------------------------
@mkdir($tmp . '/bin', 0777, true);
file_put_contents($tmp . '/bin/wdf_faces',
    "#!/bin/sh\n"
    . "if [ \"\$1\" = people ]; then\n"
    . "  echo '[{\"id\":1,\"name\":\"דנה\",\"faces\":3,\"cover\":\"crops/p1_t1_f0.jpg\"}]'\n"
    . "elif [ \"\$1\" = person ]; then\n"
    . "  echo '{\"id\":1,\"name\":\"דנה\",\"faces\":[{\"id\":5,\"bbox\":\"1,2,3,4\",\"crop\":\"crops/a.jpg\",\"photo\":\"/p/a.jpg\"}]}'\n"
    . "elif [ \"\$1\" = rename ] || [ \"\$1\" = merge ]; then\n"
    . "  exit 0\n"
    . "else\n"
    . "  exit 1\n"
    . "fi\n");
chmod($tmp . '/bin/wdf_faces', 0755);

$people = wdf_people_fetch($tmp, $tmp . '/wfi.conf');
check(is_array($people) && count($people) === 1, 'stub people fetch parses');
check($people[0]['name'] === 'דנה', 'Hebrew name survives exec+json');
$person = wdf_person_fetch($tmp, $tmp . '/wfi.conf', 1);
check(is_array($person) && $person['faces'][0]['photo'] === '/p/a.jpg',
    'stub person fetch parses');
check(wdf_people_fetch($tmp . '/nope', $tmp . '/wfi.conf') === null,
    'missing binary returns null');
check(wdf_people_fetch('/nonexistent-app', '/nope.conf') === null,
    'bad app dir returns null');

if ($fails > 0) {
    fwrite(STDERR, "{$fails} check(s) failed\n");
    exit(1);
}
echo "people_test: all checks passed\n";
