<?php
// WD Face Recognition - M1 skeleton status page.
$version = '0.1.0';
header('Content-Type: text/html; charset=utf-8');
?>
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WD Face Recognition</title>
</head>
<body>
<h1>WD Face Recognition</h1>
<p>Skeleton app installed. Version <?= htmlspecialchars($version, ENT_QUOTES, 'UTF-8') ?>.</p>
<p>Face indexing is not implemented yet; it arrives in milestone 3.
All processing in this app stays on this NAS.</p>
</body>
</html>
