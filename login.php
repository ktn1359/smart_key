<?php

require_once DIR . '/admin_auth.php';

if (admin_is_logged_in()) {
    header('Location: view.php');
    exit;
}

$error = '';

if ($_SERVER['REQUEST_METHOD'] === 'POST') {

    admin_verify_csrf();

    $username = isset($_POST['username'])
        ? trim($_POST['username'])
        : '';

    $password = isset($_POST['password'])
        ? $_POST['password']
        : '';

    $config = admin_config();

    $usernameOk =
        is_string($username) &&
        hash_equals($config['username'], $username);

    $passwordOk =
        is_string($password) &&
        password_verify($password, $config['password_hash']);

    if ($usernameOk && $passwordOk) {

        admin_login_success();

        header('Location: view.php');
        exit;

    } else {
$error = 'نام کاربری یا رمز عبور اشتباه است.';
    }
}
?>
<!DOCTYPE html>
<html lang="fa" dir="rtl">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ورود مدیر</title>
<style>
body {
    font-family: Tahoma, sans-serif;
    background: #f1f5f9;
    display: flex;
    justify-content: center;
    align-items: center;
    min-height: 100vh;
    margin: 0;
}
.box {
    width: 320px;
    background: white;
    padding: 25px;
    border-radius: 12px;
    box-shadow: 0 3px 15px #0001;
}
input {
    width: 100%;
    padding: 11px;
    margin: 8px 0;
    box-sizing: border-box;
}
button {
    width: 100%;
    padding: 11px;
    background: #0284c7;
    color: white;
    border: 0;
    border-radius: 6px;
    cursor: pointer;
}
.error {
    color: #dc2626;
    margin-bottom: 10px;
}
</style>
</head>
<body>
<div class="box">
    <h3>ورود مدیر سیستم</h3>

    <?php if ($error !== ''): ?>
        <div class="error">
            <?= htmlspecialchars($error, ENT_QUOTES, 'UTF-8') ?>
        </div>
    <?php endif; ?>

    <form method="POST">
        <input
            type="hidden"
            name="csrf_token"
            value="<?= htmlspecialchars(admin_csrf_token(), ENT_QUOTES, 'UTF-8') ?>"
        >

        <input
            type="text"
            name="username"
            placeholder="نام کاربری"
            autocomplete="username"
            required
        >

        <input
            type="password"
            name="password"
            placeholder="رمز عبور"
            autocomplete="current-password"
            required
        >

        <button type="submit">ورود</button>
    </form>
</div>
</body>
</html>
