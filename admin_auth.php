<?php

if (session_status() !== PHP_SESSION_ACTIVE) {

    ini_set('session.use_strict_mode', '1');
    ini_set('session.use_only_cookies', '1');
session_set_cookie_params(array(
        'lifetime' => 0,
        'path' => '/',
        'secure' => false,
        'httponly' => true,
        'samesite' => 'Strict'
    ));

    session_start();
}

function admin_config()
{
    static $config = null;

    if ($config === null) {
        $config = require DIR . '/admin_config.php';

        if (
            !is_array($config) ||
            empty($config['username']) ||
            empty($config['password_hash'])
        ) {
            http_response_code(500);
            exit('ADMIN_CONFIG_ERROR');
        }
    }

    return $config;
}

function admin_is_logged_in()
{
    if (empty($_SESSION['admin_authenticated'])) {
        return false;
    }

    $now = time();

    $lastActivity = isset($_SESSION['admin_last_activity'])
        ? (int)$_SESSION['admin_last_activity']
        : 0;

    $loginTime = isset($_SESSION['admin_login_time'])
        ? (int)$_SESSION['admin_login_time']
        : 0;

    // حداکثر 30 دقیقه عدم فعالیت
    if ($lastActivity <= 0 || ($now - $lastActivity) > 1800) {
        return false;
    }

    // حداکثر عمر نشست: 8 ساعت
    if ($loginTime <= 0 || ($now - $loginTime) > 28800) {
        return false;
    }

    $_SESSION['admin_last_activity'] = $now;

    return true;
}

function admin_require_login()
{
    if (!admin_is_logged_in()) {
        header('Location: login.php');
        exit;
    }
}

function admin_csrf_token()
{
    if (
        empty($_SESSION['admin_csrf']) ||
        !is_string($_SESSION['admin_csrf'])
    ) {
        $_SESSION['admin_csrf'] = bin2hex(random_bytes(32));
    }

    return $_SESSION['admin_csrf'];
}

function admin_verify_csrf()
{
    if (
        !isset($_POST['csrf_token']) ||
        !is_string($_POST['csrf_token']) ||
        empty($_SESSION['admin_csrf']) ||
        !hash_equals(
            $_SESSION['admin_csrf'],
            $_POST['csrf_token']
        )
    ) {
        http_response_code(403);
        exit('ERROR;CODE=CSRF_FAILED');
    }
}

function admin_login_success()
{
    session_regenerate_id(true);

    $_SESSION = array();

    $_SESSION['admin_authenticated'] = true;
    $_SESSION['admin_login_time'] = time();
    $_SESSION['admin_last_activity'] = time();
    $_SESSION['admin_csrf'] = bin2hex(random_bytes(32));
}

function admin_logout()
{
    $_SESSION = array();

    if (ini_get('session.use_cookies')) {
        $params = session_get_cookie_params();

        setcookie(
            session_name(),
            '',
            array(
                'expires' => time() - 42000,
                'path' => $params['path'],
                'domain' => $params['domain'],
                'secure' => $params['secure'],
                'httponly' => $params['httponly'],
                'samesite' => 'Strict'
            )
        );
    }

    session_destroy();
}