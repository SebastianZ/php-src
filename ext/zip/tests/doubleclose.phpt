--TEST--
close() called twice
--SKIPIF--
<?php
if(!extension_loaded('zip')) die('skip');
?>
--FILE--
<?php

echo "Procedural\n";
$zip = zip_open(__DIR__ . '/test.zip');
if (!is_resource($zip)) {
	die("Failure");
}
var_dump(zip_close($zip));
try {
    var_dump(zip_close($zip));
} catch (TypeError $e) {
    echo $e->getMessage(), "\n";
}

echo "Object\n";
$zip = new ZipArchive();
if (!$zip->open(__DIR__ . '/test.zip')) {
	die('Failure');
}
if ($zip->status == ZIPARCHIVE::ER_OK) {
	var_dump($zip->close());
	var_dump($zip->close());
} else {
	die("Failure");
}

?>
Done
--EXPECTF--
Procedural
NULL
zip_close(): supplied resource is not a valid Zip Directory resource
Object
bool(true)

Warning: ZipArchive::close(): Invalid or uninitialized Zip object in %s on line %d
bool(false)
Done
