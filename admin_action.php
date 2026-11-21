<?php

require_once __DIR__ . '/admin_auth.php';

admin_require_login();

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    exit('ERROR;CODE=METHOD_NOT_ALLOWED');
}

admin_verify_csrf();

define('ADMIN_STATE_FILE', __DIR__ . '/devices_state.json');
define('ADMIN_HISTORY_FILE', __DIR__ . '/packets_history.json');

/*function admin_json_update($file, $callback)
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
*/
function admin_json_update($file, $callback)
{
    $lockPath = $file . '.lock';
    $lock = @fopen($lockPath, 'c');

    if (!$lock) {
        error_log('admin_json_update: cannot open lock file: ' . $lockPath);
        return false;
    }

    if (!flock($lock, LOCK_EX)) {
        fclose($lock);
        error_log('admin_json_update: cannot lock: ' . $lockPath);
        return false;
    }

    $tmp = null;
    $out = null;
    $result = false;

    try {

        if (!is_file($file) || !is_readable($file)) {
            throw new RuntimeException('JSON file missing or unreadable');
        }

        $raw = @file_get_contents($file);

        if ($raw === false || trim($raw) === '') {
            throw new RuntimeException('JSON file is empty or unreadable');
        }

        $data = json_decode($raw, true);

        if (json_last_error() !== JSON_ERROR_NONE || !is_array($data)) {
            throw new RuntimeException(
                'Invalid JSON: ' . json_last_error_msg()
            );
        }

        $updated = $callback($data);

        if (!is_array($updated)) {
            throw new RuntimeException('Callback did not return an array');
        }

        $flags = JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES;

        $oldJson = json_encode($data, $flags);
        $newJson = json_encode($updated, $flags);

        if ($oldJson === false || $newJson === false) {
            throw new RuntimeException('JSON encoding failed');
        }

        if ($oldJson === $newJson) {
            $result = true;
        } else {

            $tmp = @tempnam(dirname($file), basename($file) . '.tmp.');

            if ($tmp === false) {
                throw new RuntimeException('Cannot create temporary file');
            }

            $out = @fopen($tmp, 'wb');

            if (!$out) {
                throw new RuntimeException('Cannot open temporary file');
            }

            $length = strlen($newJson);
            $offset = 0;

            while ($offset < $length) {

                $written = fwrite(
                    $out,
                    substr($newJson, $offset)
                );

                if ($written === false || $written === 0) {
                    throw new RuntimeException('Incomplete file write');
                }

                $offset += $written;
            }

            if (!fflush($out)) {
                throw new RuntimeException('File flush failed');
            }

            if (!fclose($out)) {
                $out = null;
                throw new RuntimeException('File close failed');
            }

            $out = null;

            $permissions = @fileperms($file);

            if ($permissions !== false) {
                @chmod($tmp, $permissions & 0777);
            }

            if (!@rename($tmp, $file)) {
                throw new RuntimeException('Atomic file replacement failed');
            }

            $tmp = null;
            $result = true;
        }

    } catch (Throwable $e) {

        error_log(
            'admin_json_update failed: ' .
            $e->getMessage() .
            ' File=' . $file
        );

        $result = false;

    } finally {

        if (is_resource($out)) {
            fclose($out);
        }

        if ($tmp !== null && is_file($tmp)) {
            @unlink($tmp);
        }

        flock($lock, LOCK_UN);
        fclose($lock);
    }

    return $result;
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

    $deviceAge = -1;

    $ok = admin_json_update(
        ADMIN_STATE_FILE,
        function (array $all) use ($mac, &$deviceAge) {

            if (
                !isset($all[$mac]) ||
                !is_array($all[$mac])
            ) {
                $deviceAge = -1;
                return $all;
            }

            $entry = $all[$mac];

            $lastSeen = isset($entry['last_seen'])
                ? (int)$entry['last_seen']
                : 0;

            $deviceAge = $lastSeen > 0
                ? max(0, time() - $lastSeen)
                : -1;

            // بررسی تازگی دستگاه در حالی که قفل فایل برقرار است.
            if ($deviceAge < 0 || $deviceAge > 20) {
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

    if ($deviceAge < 0 || $deviceAge > 20) {
        http_response_code(409);
        exit('ERROR;CODE=DEVICE_STALE');
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

    $deviceAge = -1;
    $stateLengthError = false;
    $versionError = false;

    $ok = admin_json_update(
        ADMIN_STATE_FILE,
        function (array $all) use (
            $mac,
            $set,
            &$deviceAge,
            &$stateLengthError,
            &$versionError
        ) {

            $entry = (
                isset($all[$mac]) &&
                is_array($all[$mac])
            ) ? $all[$mac] : array();

            // بررسی تازگی دستگاه داخل قفل مشترک JSON
            $lastSeen = isset($entry['last_seen'])
                ? (int)$entry['last_seen']
                : 0;
		$deviceAge = $lastSeen > 0
                ? max(0, time() - $lastSeen)
                : -1;

            if ($deviceAge < 0 || $deviceAge > 20) {
                return $all;
            }

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

    if (!$ok) {
        http_response_code(500);
        exit('ERROR;CODE=STATE_STORAGE_ERROR');
    }

    if ($deviceAge < 0 || $deviceAge > 20) {
        http_response_code(409);
        exit('ERROR;CODE=DEVICE_STALE;AGE=' . $deviceAge);
    }

    if ($stateLengthError) {
        http_response_code(400);
        exit('ERROR;CODE=BAD_STATE_LENGTH');
    }

    if ($versionError) {
        http_response_code(409);
        exit('ERROR;CODE=RELAY_VERSION_EXHAUSTED');
    }

    header('Location: view.php');
    exit;
}

http_response_code(400);
exit('ERROR;CODE=UNKNOWN_ACTION');