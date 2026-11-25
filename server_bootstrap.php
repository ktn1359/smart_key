<?php

error_reporting(E_ALL);
date_default_timezone_set('Asia/Tehran');

// مسیر فایل اصلی
$scriptPath = isset($_SERVER['SCRIPT_FILENAME'])
    ? $_SERVER['SCRIPT_FILENAME']
    : (getcwd() . '/test.php');

if (!defined('BASE_DIR')) {
    define('BASE_DIR', dirname($scriptPath));
}

if (!defined('ALLOW_LEGACY_POST')) {
    define('ALLOW_LEGACY_POST', false);
}

if (!defined('HISTORY_FILE')) {
    define('HISTORY_FILE', BASE_DIR . '/packets_history.json');
}

if (!defined('STATE_FILE')) {
    define('STATE_FILE', BASE_DIR . '/devices_state.json');
}

if (!defined('MAX_HISTORY')) {
    define('MAX_HISTORY', 200);
}

if (!defined('ADMIN_KEY')) {
    define('ADMIN_KEY', '');
}

// تنظیم مسیر لاگ
ini_set('display_errors', '0');
ini_set('log_errors', '1');
ini_set('error_log', BASE_DIR . '/error_log');

// ثبت خطاهای PHP
set_error_handler(function ($severity, $message, $file, $line) {

    if (!(error_reporting() & $severity)) {
        return false;
    }

    error_log(
        '[PHP ERROR] Severity=' . $severity .
        ' Message=' . $message .
        ' File=' . $file .
        ' Line=' . $line
    );

    return true;
});

// ثبت خطاهای Fatal
register_shutdown_function(function () {

    $e = error_get_last();

    if (
        $e !== null &&
        in_array($e['type'], array(
            E_ERROR,
            E_PARSE,
            E_CORE_ERROR,
            E_COMPILE_ERROR
        ), true)
    ) {

        error_log(
            '[PHP FATAL] Type=' . $e['type'] .
            ' Message=' . $e['message'] .
            ' File=' . $e['file'] .
            ' Line=' . $e['line']
        );

        if (!headers_sent()) {
            http_response_code(500);
            header('Content-Type: text/plain; charset=UTF-8');
        }

        echo 'ERROR;CODE=FATAL';
    }
});