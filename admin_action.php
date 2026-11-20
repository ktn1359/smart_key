<?php

require_once DIR . '/admin_auth.php';

admin_require_login();

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    exit('ERROR;CODE=METHOD_NOT_ALLOWED');
}

admin_verify_csrf();

define('ADMIN_STATE_FILE', DIR . '/devices_state.json');
define('ADMIN_HISTORY_FILE', DIR . '/packets_history.json');

function admin_json_update($file, $callback)
{
    $fp = @fopen($file, 'c+');

    if (!$fp) {
        return false;
    }

    if (!flock($fp, LOCK_EX)) {
        fclose($fp);
        return false;
    }

    rewind($fp);

    $raw = stream_get_contents($fp);

    $data = ($raw !== false && $raw !== '')
        ? json_decode($raw, true)
        : array();

    if (!is_array($data)) {
        $data = array();
    }

    $data = $callback($data);

    $json = json_encode(
        $data,
        JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES
    );

    if ($json === false) {
        flock($fp, LOCK_UN);
        fclose($fp);
        return false;
    }

    rewind($fp);
    ftruncate($fp, 0);

    $written = fwrite($fp, $json);

    fflush($fp);
    flock($fp, LOCK_UN);
    fclose($fp);

    return $written !== false;
}

function admin_clear_commands(&$entry)
{
    if (
        isset($entry['relay_commands']) &&
        is_array($entry['relay_commands'])
    ) {
        foreach ($entry['relay_commands'] as $ch => $command) {
            if (is_array($command)) {
                $entry['relay_commands'][$ch]['pending'] = false;
                $entry['relay_commands'][$ch]['cleared_at'] = time();
            }
        }
    }
}

function admin_queue_commands(&$entry, $desired)
{
    if (
        !is_string($desired) ||
        !preg_match('/^[01]{1,16}$/', $desired)
    ) {
        return false;
    }

    $reported = isset($entry['reported'])
        ? $entry['reported']
        : '';
if (
        !is_string($reported) ||
        strlen($reported) !== strlen($desired) ||
        !preg_match('/^[01]+$/', $reported)
    ) {
        return false;
    }

    $next = (
        isset($entry['relay_next_version']) &&
        is_array($entry['relay_next_version'])
    ) ? $entry['relay_next_version'] : array();

    $commands = (
        isset($entry['relay_commands']) &&
        is_array($entry['relay_commands'])
    ) ? $entry['relay_commands'] : array();

    $now = time();

    for ($ch = 0; $ch < strlen($desired); $ch++) {

        if ($desired[$ch] === $reported[$ch]) {
            continue;
        }

        $last = isset($next[$ch])
            ? (int)$next[$ch]
            : 0;

        if ($last < 0 || $last >= 4294967295) {
            return false;
        }

        $version = $last + 1;

        $next[$ch] = $version;

        $commands[$ch] = array(
            'version' => $version,
            'state' => (int)$desired[$ch],
            'pending' => true,
            'created_at' => $now
        );
    }

    $entry['relay_next_version'] = $next;
    $entry['relay_commands'] = $commands;

    return true;
}

$action = isset($_POST['action']) && is_string($_POST['action'])
    ? $_POST['action']
    : '';

if ($action === 'clear_history') {

    $ok = admin_json_update(
        ADMIN_HISTORY_FILE,
        function ($history) {
            return array();
        }
    );

    if (!$ok) {
        http_response_code(500);
        exit('ERROR;CODE=HISTORY_STORAGE_ERROR');
    }

    header('Location: view.php');
    exit;
}
if ($action === 'clear') {

    $mac = isset($_POST['mac']) && is_string($_POST['mac'])
        ? strtoupper(trim($_POST['mac']))
        : '';

    if (!preg_match('/^[0-9A-F]{2}(:[0-9A-F]{2}){5}$/', $mac)) {
        http_response_code(400);
        exit('ERROR;CODE=BAD_MAC');
    }

    $raw = @file_get_contents(ADMIN_STATE_FILE);

    $all = ($raw !== false && $raw !== '')
        ? json_decode($raw, true)
        : array();

    $entry = (
        is_array($all) &&
        isset($all[$mac]) &&
        is_array($all[$mac])
    ) ? $all[$mac] : array();

    $lastSeen = isset($entry['last_seen'])
        ? (int)$entry['last_seen']
        : 0;

    if ($lastSeen <= 0 || (time() - $lastSeen) > 20) {
        http_response_code(409);
        exit('ERROR;CODE=DEVICE_STALE');
    }

    $ok = admin_json_update(
        ADMIN_STATE_FILE,
        function (array $all) use ($mac) {

            if (
                !isset($all[$mac]) ||
                !is_array($all[$mac])
            ) {
                return $all;
            }

            $all[$mac]['desired'] = null;

            admin_clear_commands($all[$mac]);

            return $all;
        }
    );

    if (!$ok) {
        http_response_code(500);
        exit('ERROR;CODE=STATE_STORAGE_ERROR');
    }

    header('Location: view.php');
    exit;
}
if ($action === 'set') {

    $mac = isset($_POST['mac']) && is_string($_POST['mac'])
        ? strtoupper(trim($_POST['mac']))
        : '';

    $set = isset($_POST['state']) && is_string($_POST['state'])
        ? trim($_POST['state'])
        : '';

    if (!preg_match('/^[0-9A-F]{2}(:[0-9A-F]{2}){5}$/', $mac)) {
        http_response_code(400);
        exit('ERROR;CODE=BAD_MAC');
    }

    if (!preg_match('/^[01]{1,16}$/', $set)) {
        http_response_code(400);
        exit('ERROR;CODE=BAD_STATE');
    }

    // ابتدا بررسی تازگی دستگاه
    $raw = @file_get_contents(ADMIN_STATE_FILE);

    $all = ($raw !== false && $raw !== '')
        ? json_decode($raw, true)
        : array();

    $entry = (
        is_array($all) &&
        isset($all[$mac]) &&
        is_array($all[$mac])
    ) ? $all[$mac] : array();

    $lastSeen = isset($entry['last_seen'])
        ? (int)$entry['last_seen']
        : 0;

    $age = $lastSeen > 0
        ? max(0, time() - $lastSeen)
        : -1;

    if ($age < 0 || $age > 20) {
        http_response_code(409);
        exit('ERROR;CODE=DEVICE_STALE;AGE=' . $age);
    }

    $stateLengthError = false;
    $versionError = false;

    $ok = admin_json_update(
        ADMIN_STATE_FILE,
        function (array $all) use (
            $mac,
            $set,
            &$stateLengthError,
            &$versionError
        ) {
            $entry = (
                isset($all[$mac]) &&
                is_array($all[$mac])
            ) ? $all[$mac] : array();

            $reported = isset($entry['reported'])
                ? $entry['reported']
                : '';

            if (
                !is_string($reported) ||
                !preg_match('/^[01]{1,16}$/', $reported) ||
                strlen($reported) !== strlen($set)
            ) {
                $stateLengthError = true;
                return $all;
            }

            if (!admin_queue_commands($entry, $set)) {
                $versionError = true;
                return $all;
            }

            // حفظ سازگاری با مسیر Legacy
            $entry['desired'] = $set;

            $all[$mac] = $entry;

            return $all;
        }
    );

    if ($stateLengthError) {
        http_response_code(400);
        exit('ERROR;CODE=BAD_STATE_LENGTH');
    }

    if ($versionError) {
        http_response_code(409);
        exit('ERROR;CODE=RELAY_VERSION_EXHAUSTED');
    }
if (!$ok) {
        http_response_code(500);
        exit('ERROR;CODE=STATE_STORAGE_ERROR');
    }

    header('Location: view.php');
    exit;
}

http_response_code(400);
exit('ERROR;CODE=UNKNOWN_ACTION');