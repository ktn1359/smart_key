<?php

// -----------------------------------------------------
// Device State
// مدیریت وضعیت جاری دستگاه‌ها
// -----------------------------------------------------

function apply_relay_push_state(
    array $all,
    $mac,
    $channel,
    $state,
    $counter,
    &$response,
    &$counterAccepted,
    &$serverLastCounter
) {
    $entry = (isset($all[$mac]) && is_array($all[$mac]))
        ? $all[$mac]
        : array();

    // بررسی Counter تکراری یا قدیمی
    if (isset($entry['last_counter'])) {
        $lastCounter = $entry['last_counter'];
        $serverLastCounter = $lastCounter;

        if (
            !is_valid_counter($lastCounter) ||
            decimal_compare($counter, $lastCounter) <= 0
        ) {
            $counterAccepted = false;
            return $all;
        }
    }

    if (
        !isset($entry['reported']) ||
        !is_string($entry['reported']) ||
        !preg_match('/^[01]+$/', $entry['reported'])
    ) {
        $counterAccepted = false;
        return $all;
    }

    $reported = $entry['reported'];

    if ($channel >= strlen($reported)) {
        $counterAccepted = false;
        return $all;
    }

    // وضعیت واقعی گزارش‌شده از دستگاه
    $reported[$channel] = (string)$state;

    // تغییر محلی دستگاه باید فرمان معوق همان کانال را خنثی کند.
    $desired = isset($entry['desired'])
        ? $entry['desired']
        : null;

    if (
        is_string($desired) &&
        strlen($desired) === strlen($reported) &&
        preg_match('/^[01]+$/', $desired)
    ) {
        $desired[$channel] = (string)$state;

        if ($desired === $reported) {
            $desired = null;
        }
    } else {
        $desired = null;
    }

    $entry['reported'] = $reported;
    $entry['desired'] = $desired;
    $entry['last_seen'] = time();
    $entry['last_counter'] = $counter;

    $response = $reported;
    $all[$mac] = $entry;

    return $all;
}