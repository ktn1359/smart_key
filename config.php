<?php

if (!defined('DIR')) {
    define('DIR', __DIR__);
}

define('APP_ROOT', __DIR__);

define('DB_DIRECTORY', APP_ROOT . '/private_data');

define('DB_FILE', DB_DIRECTORY . '/kp4000.sqlite');

define('DB_SCHEMA_VERSION', 1);

date_default_timezone_set('Asia/Tehran');