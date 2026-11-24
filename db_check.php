<?php

require_once __DIR__ . '/config.php';
require_once DIR . '/admin_auth.php';

admin_require_login();

header('Cache-Control: no-store, no-cache, must-revalidate');
header('Pragma: no-cache');
header('Content-Type: text/plain; charset=UTF-8');

try {

    require_once DIR . '/database.php';

    $pdo = app_db();

    $sqliteVersion = $pdo
        ->query('SELECT sqlite_version()')
        ->fetchColumn();

    $journalMode = $pdo
        ->query('PRAGMA journal_mode')
        ->fetchColumn();

    $schemaVersion = $pdo
        ->query('PRAGMA user_version')
        ->fetchColumn();

    $tables = $pdo->query("
        SELECT name
        FROM sqlite_master
        WHERE type = 'table'
        AND name NOT LIKE 'sqlite_%'
        ORDER BY name
    ")->fetchAll(PDO::FETCH_COLUMN);

    $columns = $pdo->query("
        PRAGMA table_info(devices)
    ")->fetchAll();

    $hasProjectName = false;

    foreach ($columns as $column) {
        if ($column['name'] === 'project_name') {
            $hasProjectName = true;
            break;
        }
    }
echo "SQLITE_STATUS=OK\n";
    echo "PDO_SQLITE=YES\n";
    echo "SQLITE_VERSION=" . $sqliteVersion . "\n";
    echo "JOURNAL_MODE=" . $journalMode . "\n";
    echo "SCHEMA_VERSION=" . $schemaVersion . "\n";
    echo "TABLES=" . implode(',', $tables) . "\n";
    echo "PROJECT_NAME=" .
        ($hasProjectName ? 'YES' : 'NO') . "\n";

    echo "DATABASE_FILE=" .
        (file_exists(DB_FILE) ? 'EXISTS' : 'MISSING') . "\n";

} catch (Throwable $e) {

    error_log(
        '[SQLITE CHECK] ' . $e->getMessage()
    );

    http_response_code(500);

    echo "SQLITE_STATUS=ERROR\n";
    echo "CHECK_ERROR_LOG\n";
}