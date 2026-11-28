<?php

// -----------------------------------------------------
// Packet Parser
// تجزیه بسته باینری دستگاه KP-4000
// -----------------------------------------------------

function parse_packet($raw)
{
    $len = strlen($raw);

    if ($len < 15) {
        fail(400, 'SHORT_PACKET', 'LENGTH=' . $len);
    }

    $mac = strtoupper(
        implode(':', str_split(bin2hex(substr($raw, 0, 6)), 2))
    );

    $ip = ord($raw[6]) . '.' .
          ord($raw[7]) . '.' .
          ord($raw[8]) . '.' .
          ord($raw[9]);

    $deviceInfo = ord($raw[10]);
    $deviceInfo2 = ord($raw[11]);

    $deviceType = $deviceInfo & 0x0F;
    $relayCap = ($deviceInfo >> 4) & 0x03;
    $touchCap = ($deviceInfo >> 6) & 0x03;

    $relayCount = decode_channels($relayCap);
    $touchCount = decode_channels($touchCap);

    if ($relayCount === 0 || $touchCount === 0) {
        fail(
            400,
            'UNSUPPORTED_CAPABILITY',
            'RELAY_CAP=' . $relayCap .
            ';TOUCH_CAP=' . $touchCap
        );
    }

    $relayBytes = ($relayCount + 7) >> 3;
    $touchBytes = ($touchCount + 7) >> 3;

    $relayOffset = 12;
    $touchOffset = 12 + $relayBytes;
    $countOffset = 12 + $relayBytes + $touchBytes;
    $headerLen = $countOffset + 1;

    if ($len < $headerLen) {
        fail(
            400,
            'SHORT_PACKET',
            'LENGTH=' . $len . ';HEADER=' . $headerLen
        );
    }

    $timerCount = ord($raw[$countOffset]);
$legacyExpected = $headerLen + ($timerCount * 12);
    $newExpected = $legacyExpected + ($relayCount * 4);

    // ACKVER در پروتکل نهایی اجباری است.
    $hasAckVersion = true;

    if ($len !== $newExpected) {
        fail(
            400,
            'BAD_LENGTH',
            'EXPECTED=' . $newExpected .
            ';RECEIVED=' . $len
        );
    }

    $relays = bitmap_to_array(
        $raw,
        $relayOffset,
        $relayCount
    );

    $touches = bitmap_to_array(
        $raw,
        $touchOffset,
        $touchCount
    );

    $timers = array();

    for ($i = 0; $i < $timerCount; $i++) {

        $o = $headerLen + $i * 12;

        $idByte = ord($raw[$o]);
        $typeStatus = ord($raw[$o + 1]);
        $weekMask = ord($raw[$o + 2]);

        $timers[] = array(
            'index' => $i,
            'relay_id' => ($idByte >> 4) & 0x0F,
            'timer_id' => $idByte & 0x0F,
            'type' => $typeStatus & 0x0F,
            'status' => ($typeStatus >> 4) & 0x0F,
            'daily' => (($weekMask & 0x80) !== 0),
            'week_mask' => $weekMask & 0x7F,
            'week_mask_raw' => $weekMask,
            'start_hour' => ord($raw[$o + 3]),
            'start_minute' => ord($raw[$o + 4]),
            'time_on' => u24le($raw, $o + 5),
            'time_off' => u24le($raw, $o + 8),
            'count' => ord($raw[$o + 11])
        );
    }

    $ackVersions = array();

    if ($hasAckVersion) {

        $ackOffset = $legacyExpected;

        $ackRaw = substr(
            $raw,
            $ackOffset,
            $relayCount * 4
        );

        $unpacked = unpack('V*', $ackRaw);

        if (!is_array($unpacked) ||
            count($unpacked) !== $relayCount) {
            fail(400, 'BAD_ACKVER');
        }

        foreach ($unpacked as $version) {
            $ackVersions[] = (string)$version;
        }
    }

    return array(
        'mac' => $mac,
        'ip' => $ip,
        'device_info' => $deviceInfo,
        'device_info_2' => $deviceInfo2,
        'device_type' => $deviceType,
        'relay_cap' => $relayCap,
        'touch_cap' => $touchCap,
        'relay_count' => $relayCount,
        'touch_count' => $touchCount,
        'relays' => $relays,
        'touches' => $touches,
        'relay_raw_hex' => strtoupper(
            bin2hex(substr($raw, $relayOffset, $relayBytes))
        ),
        'touch_raw_hex' => strtoupper(
            bin2hex(substr($raw, $touchOffset, $touchBytes))
        ),
        'timer_count' => $timerCount,
        'has_ackver' => $hasAckVersion,
        'ack_versions' => $ackVersions,
        'timers' => $timers
    );
}