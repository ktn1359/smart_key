<?php

// -----------------------------------------------------
// Protocol Helpers
// توابع عمومی و کمکی پروتکل KP-4000
// -----------------------------------------------------

function decode_channels($code)
{
    $map = array(0 => 4, 1 => 8, 2 => 16);
    return isset($map[$code]) ? $map[$code] : 0;
}

function decimal_compare($a, $b)
{
    $a = ltrim((string)$a, '0');
    $b = ltrim((string)$b, '0');

    if ($a === '') $a = '0';
    if ($b === '') $b = '0';

    if (strlen($a) < strlen($b)) return -1;
    if (strlen($a) > strlen($b)) return 1;

    return strcmp($a, $b);
}

function is_valid_counter($counter)
{
    if (!is_string($counter) ||
        !preg_match('/^[1-9][0-9]{0,19}$/', $counter)) {
        return false;
    }

    return decimal_compare(
        $counter,
        '18446744073709551615'
    ) <= 0;
}

function bitmap_to_array($raw, $offset, $count)
{
    $out = array();

    for ($ch = 0; $ch < $count; $ch++) {
        $byte = ord($raw[$offset + ($ch >> 3)]);
        $out[$ch] = ($byte >> ($ch & 7)) & 1;
    }

    return $out;
}

// عدد 24 بیتی Little-Endian
function u24le($raw, $offset)
{
    return ord($raw[$offset]) |
        (ord($raw[$offset + 1]) << 8) |
        (ord($raw[$offset + 2]) << 16);
}