<?php

// -----------------------------------------------------
// Security HMAC
// مدیریت Secret و اعتبارسنجی امضای دستگاه KP-4000
// -----------------------------------------------------

function load_device_secret($mac)
{
    $secrets = require BASE_DIR . '/device_secrets.php';

    if (!is_array($secrets) || !isset($secrets[$mac])) {
        fail(401, 'UNKNOWN_DEVICE');
    }

    $secretHex = $secrets[$mac];

    if (!is_string($secretHex) ||
        !preg_match('/^[0-9A-Fa-f]{64}$/', $secretHex)) {
        fail(500, 'INVALID_DEVICE_SECRET');
    }

    $secret = hex2bin($secretHex);

    if ($secret === false) {
        fail(500, 'INVALID_DEVICE_SECRET');
    }

    return $secret;
}

function verify_device_hmac($secret, $counter, $payload, $signature)
{
    $signingData =
        "DEVICE_TO_SERVER" . "\0" .
        $counter . "\0" .
        $payload;

    $expectedHmac = hash_hmac(
        'sha256',
        $signingData,
        $secret,
        true
    );

    return hash_equals($expectedHmac, $signature);
}