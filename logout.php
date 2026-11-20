<?php

require_once DIR . '/admin_auth.php';

admin_require_login();

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    exit('METHOD_NOT_ALLOWED');
}

admin_verify_csrf();

admin_logout();

header('Location: login.php');
exit;