<?php
// Serves one indexer face crop (issue #50, 0.3.0 PR 3d). Crops are private
// face data, so this is PIN-gated like results.php and the basename-only
// allowlist keeps the read inside data_dir/crops.
require_once __DIR__ . '/auth.php';
wdf_session_start();
require_once __DIR__ . '/paths.php';
require_once __DIR__ . '/people_lib.php';

$appDir = realpath(dirname(__DIR__));
$configPath = wdf_wfi_config_path($appDir);
$config = wdf_wfi_config_load($configPath);
$dataDir = is_array($config) ? ($config['data_dir'] ?? '') : '';

$reportsDir = '/shares/facerecog-reports';
$pinOverride = getenv('WDF_PIN_FILE');
$pinFile = ($pinOverride !== false && $pinOverride !== '')
    ? $pinOverride : $reportsDir . '/demo-pin.txt';

if (!wdf_pin_session_ok($pinFile)) {
    http_response_code(403);
    exit;
}
$crop = $dataDir !== ''
    ? wdf_safe_crop_path($dataDir . '/crops', (string)($_GET['f'] ?? ''))
    : null;
if ($crop === null) {
    http_response_code(404);
    exit;
}
header('Content-Type: image/jpeg');
header('Cache-Control: private, max-age=3600');
readfile($crop);
