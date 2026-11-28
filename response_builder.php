<?php

// -----------------------------------------------------
// Response Builder
// ساخت پاسخ امضاشده سرور برای دستگاه
// -----------------------------------------------------

function build_signed_device_response($counter, $relay, $cmdverText, $secret)
{
    $responseSigningData =
        "SERVER_TO_DEVICE" . "\0" .
        $counter . "\0" .
        $relay . "\0" .
        $cmdverText;

    $responseSignature = hash_hmac(
        'sha256',
        $responseSigningData,
        $secret,
        true
    );

    return
        'COUNTER=' . $counter .
        '&RELAY=' . $relay .
        '&CMDVER=' . $cmdverText .
        '&SIGNATURE=' . strtoupper(bin2hex($responseSignature));
}