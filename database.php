<?php

requiDIR __DIR__ . '/config.php';

function app_db()
{
    static $pdo = null;

    if ($pdo instanceof PDO) {
        return $pdo;
    }

    if (!extension_loaded('pdo_sqlite')) {
        throw new RuntimeException('PDO_SQLITE_MISSING');
    }

    if (!is_dir(DB_DIRECTORY)) {
        throw new RuntimeException('DB_DIRECTORY_MISSING');
    }

    if (!is_writable(DB_DIRECTORY)) {
        throw new RuntimeException('DB_DIRECTORY_NOT_WRITABLE');
    }

    $pdo = new PDO(
        'sqlite:' . DB_FILE,
        null,
        null,
        array(
            PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
            PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
            PDO::ATTR_EMULATE_PREPARES => false
        )
    );

    $pdo->exec('PRAGMA busy_timeout = 5000');
    $pdo->exec('PRAGMA foreign_keys = ON');
    $pdo->exec('PRAGMA journal_mode = WAL');
    $pdo->exec('PRAGMA synchronous = NORMAL');

    $version = (int)$pdo
        ->query('PRAGMA user_version')
        ->fetchColumn();

    if ($version > DB_SCHEMA_VERSION) {
        throw new RuntimeException('DB_SCHEMA_TOO_NEW');
    }

    if ($version < 1) {

        $pdo->beginTransaction();

        try {

            $pdo->exec("
                CREATE TABLE IF NOT EXISTS devices (

                    mac TEXT PRIMARY KEY COLLATE NOCASE,

                    project_name TEXT NOT NULL DEFAULT '',

                    ip TEXT NOT NULL DEFAULT '',

                    device_info INTEGER,
                    device_info_2 INTEGER,
                    device_type INTEGER,

                    relay_cap INTEGER,
                    touch_cap INTEGER,

                    relay_count INTEGER,
                    touch_count INTEGER,

                    relays_json TEXT NOT NULL DEFAULT '[]',
                    touches_json TEXT NOT NULL DEFAULT '[]',

                    relay_raw_hex TEXT,
                    touch_raw_hex TEXT,

                    timer_count INTEGER,
                    has_ackver INTEGER,

                    timers_json TEXT NOT NULL DEFAULT '[]',
                    ack_versions_json TEXT NOT NULL DEFAULT '[]',

                    reported TEXT NOT NULL DEFAULT '',
                    desired TEXT,

                    last_counter TEXT NOT NULL DEFAULT '0',
                    last_seen INTEGER NOT NULL DEFAULT 0,

                    relay_next_version_json
                        TEXT NOT NULL DEFAULT '[]',

                    relay_commands_json
                        TEXT NOT NULL DEFAULT '{}',

                    last_packet_json TEXT NOT NULL DEFAULT '{}',
packet_length INTEGER,
                    raw_hex TEXT,
                    remote_addr TEXT,

                    extra_data_json TEXT NOT NULL DEFAULT '{}',

                    created_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL
                )
            ");

            $pdo->exec("
                CREATE TABLE IF NOT EXISTS device_history (

                    id INTEGER PRIMARY KEY AUTOINCREMENT,

                    mac TEXT NOT NULL,

                    event_time INTEGER NOT NULL,

                    event_type TEXT NOT NULL,

                    source TEXT NOT NULL,

                    changed_fields_json
                        TEXT NOT NULL DEFAULT '[]',

                    old_values_json TEXT,
                    new_values_json TEXT,

                    old_ip TEXT,
                    new_ip TEXT,

                    packet_json TEXT,

                    remote_addr TEXT,

                    FOREIGN KEY (mac)
                        REFERENCES devices(mac)
                        ON DELETE RESTRICT
                )
            ");

            $pdo->exec("
                CREATE INDEX IF NOT EXISTS
                idx_devices_last_seen
                ON devices(last_seen)
            ");

            $pdo->exec("
                CREATE INDEX IF NOT EXISTS
                idx_history_mac_id
                ON device_history(mac, id DESC)
            ");

            $pdo->exec("
                CREATE INDEX IF NOT EXISTS
                idx_history_event_time
                ON device_history(event_time DESC)
            ");

            $pdo->exec("
                CREATE INDEX IF NOT EXISTS
                idx_history_event_type
                ON device_history(event_type)
            ");

            $pdo->exec('PRAGMA user_version = 1');

            $pdo->commit();

        } catch (Throwable $e) {

            if ($pdo->inTransaction()) {
                $pdo->rollBack();
            }

            throw $e;
        }
    }

    return $pdo;
}
