<?php
 

$packet1 = $_GET['packet'] ??'Get_packet=nodata';
$packet2 = $_POST['packet'] ??'Post_packet=nodata';
 
echo $packet1;
echo $packet2;

// _______________________________


 
?>


<?php
/**
 * Smart Home Binary Packet Parser (14 Bytes Header + 12*N Bytes Timers)
 * Developer: Rozhina
 */
 
date_default_timezone_set('Asia/Tehran');


// ۱. دریافت داده باینری خام از HTTP Body یا متغیر تست
$rawPayload = file_get_contents('php://input');


if (empty($raw) && isset($_GET['packet'])) {
    $raw = $_GET['packet'];
}


if (empty($raw)) {
    http_response_code(400);
    echo "packet=nodata";
    exit;
}

$len = strlen($raw);
if ($len < 14) {
    http_response_code(400);
    echo "packet=short_header";
    exit;
}

// 1. MAC Address
$mac = sprintf('%02X:%02X:%02X:%02X:%02X:%02X',
    ord($raw[0]), ord($raw[1]), ord($raw[2]),
    ord($raw[3]), ord($raw[4]), ord($raw[5])
);

// 2. IP Address
$ip = sprintf('%d.%d.%d.%d',
    ord($raw[6]), ord($raw[7]), ord($raw[8]), ord($raw[9])
);


// 3. Device Info
$deviceInfo = ord($raw[10]);
$deviceType = $deviceInfo & 0x0F;
$relayCap   = ($deviceInfo >> 4) & 0x03;
$touchCap   = ($deviceInfo >> 6) & 0x03;

// 4. Relay Status (8 Relays)
$relayByte = ord($raw[11]);
$relays = [];
for ($i = 0; $i < 8; $i++) {
    $relays[$i] = ($relayByte >> $i) & 0x01;
}

// 5. Touch Status (8 Touches)
$touchByte = ord($raw[12]);
$touches = [];
for ($i = 0; $i < 8; $i++) {
    $touches[$i] = ($touchByte >> $i) & 0x01;
}

// 6. Timers Count
$timerCount = ord($raw[13]);

// 7. Timer Records
$timers = [];
$offset = 14;

for ($i = 0; $i < $timerCount; $i++) {
    if ($offset + 12 > $len) break;

    $rec = substr($raw, $offset, 12);
    $offset += 12;

    $timerIdByte = ord($rec[0]);
    $typeStatus  = ord($rec[1]);

    $timers[] = [
        'relay_id'     => ($timerIdByte >> 4) & 0x0F,
        'timer_id'     => $timerIdByte & 0x0F,
        'type'         => $typeStatus & 0x0F,
        'status'       => ($typeStatus >> 4) & 0x0F,
        'week_mask'    => ord($rec[2]),
        'start_hour'   => ord($rec[3]),
        'start_minute' => ord($rec[4]),
        'time_on'      => (ord($rec[5]) << 16) | (ord($rec[6]) << 8) | ord($rec[7]),
        'time_off'     => (ord($rec[8]) << 16) | (ord($rec[9]) << 8) | ord($rec[10]),
        'count'        => ord($rec[11])
    ];
}

// ساخت آرایه رکورد برای ذخیره
$record = [
    'received_at'  => date('Y-m-d H:i:s'),
    'timestamp'    => time(),
    'raw_hex'      => bin2hex($raw),
    'mac'          => $mac,
    'ip'           => $ip,
    'device_type'  => $deviceType,
    'relay_cap'    => $relayCap,
    'touch_cap'    => $touchCap,
    'relay_byte'   => $relayByte,
    'relays'       => $relays,
    'touch_byte'   => $touchByte,
    'touches'      => $touches,
    'timer_count'  => $timerCount,
    'timers'       => $timers
];

// ذخیره‌سازی در فایل JSON (حفظ آخرین ۲۰۰ بسته با قفل فایل جهت جلوگیری از همزمانی)
$storageFile = __DIR__ . '/packets_history.json';
$history = [];

if (file_exists($storageFile)) {
    $existing = @file_get_contents($storageFile);
    if (!empty($existing)) {
        $decoded = json_decode($existing, true);
        if (is_array($decoded)) {
            $history = $decoded;
        }
    }
}

// افزودن به ابتدای لیست (جدیدترین‌ها اول)
array_unshift($history, $record);
if (count($history) > 200) {
    $history = array_slice($history, 0, 200);
}

// نوشتن با File Lock
file_put_contents($storageFile, json_encode($history, JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE), LOCK_EX);

// پاسخ به سخت‌افزار
header("Content-Type: text/plain; charset=UTF-8");
echo "OK;MAC=" . $mac . ";COUNT=" . $timerCount;