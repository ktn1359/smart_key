<?php
/*
 * test.php  -  دریافت «بسته‌ی وضعیت باینری» دستگاه (HTTP POST) و پاسخ‌دادن به آن
 *
 * ساختار بسته (از CloudAPI_1_1.ino: cloudBuildStatusPacket):
 *
 *   بایت‌های 0..5   : MAC (خام، 6 بایت)
 *   بایت‌های 6..9   : IPv4 دستگاه (4 بایت)
 *   بایت 10         : DeviceInfo  = نوع دستگاه (4 بیت پایین)
 *                                   | کد تعداد رله  << 4   (0=4، 1=8، 2=16)
 *                                   | کد تعداد تاچ   << 6   (0=4، 1=8، 2=16)
 *   بعد از آن       : بیت‌مپ رله‌ها   (1 بایت تا 8 کانال، 2 بایت برای 9..16 کانال) - بیت i = کانال i
 *                     بیت‌مپ تاچ‌ها   (هم‌اندازه‌ی بیت‌مپ رله‌ها)
 *                     تعداد رکورد تایمر (1 بایت)
 *   سپس             : N رکورد 12 بایتی تایمر:
 *        [0] timerId   = (relay << 4) | timerIndex
 *        [1] typeStatus= (status << 4) | type
 *        [2] weekMask  (0x80 = روزانه؛ وگرنه ماسک روزهای هفته؛ 0 = تک‌باره)
 *        [3] ساعت شروع     [4] دقیقه‌ی شروع
 *        [5..7]  timeOn  = عدد 24 بیتی LITTLE-ENDIAN (بایت کم‌ارزش اول)
 *        [8..10] timeOff = عدد 24 بیتی LITTLE-ENDIAN
 *        [11] count
 *
 * پاسخ مورد انتظار دستگاه (cloudProcessCompletedRequest):
 *   HTTP 200 و بدنه فقط یک رشته از '0' و '1' به تعداد رله‌ها، مثلاً  0101
 *   (کانال i = کاراکتر i = وضعیتی که سرور می‌خواهد رله i داشته باشد).
 *   هر متن دیگر (مثل OK;...) به‌عنوان «poll failed» حساب می‌شود.
 *
 * رفتار پیش‌فرض این تست: وضعیت گزارش‌شده‌ی خود دستگاه برگردانده می‌شود (یعنی سرور هیچ تغییری
 * فرمان نمی‌دهد). برای فرمان دادن در تست:
 *      test.php?mac=AA:BB:CC:DD:EE:FF&set=0101      (برای پاک کردن: set=clear)
 * فرمان تا وقتی دستگاه آن را اعمال نکرده برگردانده می‌شود، بعد خودش پاک می‌شود.
 * آخرین بسته‌ی دریافتی:   test.php?last=1
 *
 * نکته: فایل را با کدگذاری UTF-8 «بدون BOM» ذخیره کن و بعد از <?php و قبل از آن هیچ
 * کاراکتر اضافه نگذار؛ تگ پایانی بسته نشده تا خروجی اضافه‌ای (فاصله/خط خالی) به پاسخ نرسد.
 */

error_reporting(E_ALL);
ini_set('display_errors', '0');   // خطای PHP هرگز داخل پاسخ دستگاه چاپ نشود
ini_set('log_errors', '1');
date_default_timezone_set('Asia/Tehran');

// پوشه‌ی همین فایل (بدون استفاده از ثابت‌های جادویی PHP که ویرایشگرها/چت ممکنه خرابشون کنن)
$scriptPath = isset($_SERVER['SCRIPT_FILENAME']) ? $_SERVER['SCRIPT_FILENAME'] : (getcwd() . '/test.php');

define('BASE_DIR',     dirname($scriptPath));
define('HISTORY_FILE', BASE_DIR . '/packets_history.json');   // آخرین بسته‌ها
define('STATE_FILE',   BASE_DIR . '/devices_state.json');     // وضعیت هر دستگاه + فرمان در انتظار
define('MAX_HISTORY',  200);
define('ADMIN_KEY',    '');   // اگر خالی نباشد، ?set و ?last فقط با &key=... کار می‌کنند

// ---------------------------------------------------------------------
// اگر خطای مرگبار PHP رخ داد، حداقل یک بدنه‌ی متنی قابل‌فهم برگردد (نه پاسخ خالی)
// ---------------------------------------------------------------------
register_shutdown_function(function () {
    $e = error_get_last();

    if ($e !== null && in_array($e['type'], array(E_ERROR, E_PARSE, E_CORE_ERROR, E_COMPILE_ERROR), true)) {
        if (!headers_sent()) {
            http_response_code(500);
            header('Content-Type: text/plain; charset=UTF-8');
        }
        echo 'ERROR;CODE=FATAL;MSG=' . clean_msg($e['message']);
    }
});

function clean_msg($s)
{
    return substr(preg_replace('/[^\x20-\x7E]/', '?', str_replace(array(';', "\r", "\n"), ' ', (string)$s)), 0, 120);
}

function respond($code, $body)
{
    http_response_code($code);
    header('Content-Type: text/plain; charset=UTF-8');
    header('Cache-Control: no-store');
    echo $body;
    exit;
}

function fail($code, $name, $extra = '')
{
    respond($code, 'ERROR;CODE=' . $name . ($extra !== '' ? ';' . $extra : ''));
}

// ---------------------------------------------------------------------
// خواندن/نوشتن امن فایل JSON با قفل (چند درخواست هم‌زمان فایل را خراب نکنند)
// $callback(array) -> array
// ---------------------------------------------------------------------
function json_update($file, $callback)
{
    $fp = @fopen($file, 'c+');

    if (!$fp) {
        error_log('test.php: cannot open ' . $file);
        return false;
    }

    if (!flock($fp, LOCK_EX)) {
        fclose($fp);
        return false;
    }

    $raw  = stream_get_contents($fp);
    $data = ($raw !== false && $raw !== '') ? json_decode($raw, true) : array();

    if (!is_array($data)) {
        $data = array();
    }

    $data = $callback($data);
    $json = json_encode($data, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);

    if ($json === false) {
        flock($fp, LOCK_UN);
        fclose($fp);
        return false;
    }

    rewind($fp);
    ftruncate($fp, 0);
    $ok = (fwrite($fp, $json) !== false);
    fflush($fp);
    flock($fp, LOCK_UN);
    fclose($fp);

    return $ok;
}

// ---------------------------------------------------------------------
// کمکی‌های رمزگشایی
// ---------------------------------------------------------------------
function decode_channels($code)
{
    $map = array(0 => 4, 1 => 8, 2 => 16);
    return isset($map[$code]) ? $map[$code] : 0;
}

function bitmap_to_array($raw, $offset, $count)
{
    $out = array();

    for ($ch = 0; $ch < $count; $ch++) {
        $byte    = ord($raw[$offset + ($ch >> 3)]);
        $out[$ch] = ($byte >> ($ch & 7)) & 1;
    }

    return $out;
}

// عدد 24 بیتی Little-Endian (timerWriteU24LE در دستگاه)
function u24le($raw, $offset)
{
    return ord($raw[$offset]) | (ord($raw[$offset + 1]) << 8) | (ord($raw[$offset + 2]) << 16);
}

function parse_packet($raw)
{
    $len = strlen($raw);

    if ($len < 14) {
        fail(400, 'SHORT_PACKET', 'LENGTH=' . $len);
    }

    $mac = strtoupper(implode(':', str_split(bin2hex(substr($raw, 0, 6)), 2)));
    $ip  = ord($raw[6]) . '.' . ord($raw[7]) . '.' . ord($raw[8]) . '.' . ord($raw[9]);

    $deviceInfo = ord($raw[10]);
    $deviceType = $deviceInfo & 0x0F;
    $relayCap   = ($deviceInfo >> 4) & 0x03;
    $touchCap   = ($deviceInfo >> 6) & 0x03;

    $relayCount = decode_channels($relayCap);
    $touchCount = decode_channels($touchCap);

    if ($relayCount === 0 || $touchCount === 0) {
        fail(400, 'UNSUPPORTED_CAPABILITY', 'RELAY_CAP=' . $relayCap . ';TOUCH_CAP=' . $touchCap);
    }

    $relayBytes = ($relayCount + 7) >> 3;
    $touchBytes = ($touchCount + 7) >> 3;

    $relayOffset = 11;
    $touchOffset = 11 + $relayBytes;
    $countOffset = 11 + $relayBytes + $touchBytes;
    $headerLen   = $countOffset + 1;

    if ($len < $headerLen) {
        fail(400, 'SHORT_PACKET', 'LENGTH=' . $len . ';HEADER=' . $headerLen);
    }

    $timerCount = ord($raw[$countOffset]);
    $expected   = $headerLen + $timerCount * 12;

    if ($len !== $expected) {
        fail(400, 'BAD_LENGTH',
            'EXPECTED=' . $expected . ';RECEIVED=' . $len .
            ';HEADER_HEX=' . strtoupper(bin2hex(substr($raw, 0, $headerLen))));
    }

    $relays  = bitmap_to_array($raw, $relayOffset, $relayCount);
    $touches = bitmap_to_array($raw, $touchOffset, $touchCount);

    $timers = array();

    for ($i = 0; $i < $timerCount; $i++) {
        $o          = $headerLen + $i * 12;
        $idByte     = ord($raw[$o]);
        $typeStatus = ord($raw[$o + 1]);
        $weekMask   = ord($raw[$o + 2]);

        $timers[] = array(
            'index'        => $i,
            'relay_id'     => ($idByte >> 4) & 0x0F,
            'timer_id'     => $idByte & 0x0F,
            'type'         => $typeStatus & 0x0F,
            'status'       => ($typeStatus >> 4) & 0x0F,
            'daily'        => (($weekMask & 0x80) !== 0),
            'week_mask'    => $weekMask & 0x7F,
            'week_mask_raw' => $weekMask,
            'start_hour'   => ord($raw[$o + 3]),
            'start_minute' => ord($raw[$o + 4]),
            'time_on'      => u24le($raw, $o + 5),
            'time_off'     => u24le($raw, $o + 8),
            'count'        => ord($raw[$o + 11])
        );
    }

    return array(
        'mac'          => $mac,
        'ip'           => $ip,
        'device_info'  => $deviceInfo,
        'device_type'  => $deviceType,
        'relay_cap'    => $relayCap,
        'touch_cap'    => $touchCap,
        'relay_count'  => $relayCount,
        'touch_count'  => $touchCount,
        'relays'       => $relays,
        'touches'      => $touches,
        'timer_count'  => $timerCount,
        'timers'       => $timers
    );
}

// ---------------------------------------------------------------------
// دریافت بسته از دستگاه
// ---------------------------------------------------------------------
function handle_device_post()
{
$raw = file_get_contents('php://input');

if ($raw === false || $raw === '') {
    fail(400, 'NO_DATA');
}

// HMAC-SHA256: last 32 bytes
if (strlen($raw) < 33) {
    fail(400, 'SHORT_SIGNED_PACKET');
}

$payload = substr($raw, 0, -32);
$receivedHmac = substr($raw, -32);

// Extract MAC from original packet
$mac = strtoupper(
    implode(':', str_split(bin2hex(substr($payload, 0, 6)), 2))
);

// Load device secrets
$secrets = require __DIR__ . '/device_secrets.php';

if (!isset($secrets[$mac])) {
    fail(401, 'UNKNOWN_DEVICE');
}

$secretHex = $secrets[$mac];

if (!preg_match('/^[0-9A-Fa-f]{64}$/', $secretHex)) {
    fail(500, 'INVALID_DEVICE_SECRET');
}

$secret = hex2bin($secretHex);

// Calculate expected HMAC
$expectedHmac = hash_hmac(
    'sha256',
    $payload,
    $secret,
    true
);

// Verify signature
if (!hash_equals($expectedHmac, $receivedHmac)) {
    fail(401, 'INVALID_HMAC');
}

// Parse original 110-byte packet
$p = parse_packet($payload);

$reported = implode('', $p['relays']);      

    $reported = implode('', $p['relays']);

    $record = array_merge(array(
        'received_at'   => date('Y-m-d H:i:s'),
        'timestamp'     => time(),
        'remote_addr'   => isset($_SERVER['REMOTE_ADDR']) ? $_SERVER['REMOTE_ADDR'] : '',
        'packet_length' => strlen($raw),
        'raw_hex'       => strtoupper(bin2hex($raw)),
        'relay_string'  => $reported
    ), $p);

    // 1) تاریخچه (خطای ذخیره‌سازی نباید پاسخ دستگاه را خراب کند)
    $historyOk = json_update(HISTORY_FILE, function (array $h) use ($record) {
        array_unshift($h, $record);
        return array_slice($h, 0, MAX_HISTORY);
    });

    // 2) وضعیت دستگاه و تعیین پاسخ
    $mac      = $p['mac'];
    $response = $reported;   // پیش‌فرض: همان وضعیتی که دستگاه گزارش کرده (بدون فرمان)
    $ip       = $p['ip'];

    $stateOk = json_update(STATE_FILE, function (array $all) use ($mac, $reported, $ip, &$response) {
        $entry   = (isset($all[$mac]) && is_array($all[$mac])) ? $all[$mac] : array();
        $desired = isset($entry['desired']) ? $entry['desired'] : null;

        if (is_string($desired) && strlen($desired) === strlen($reported) && preg_match('/^[01]+$/', $desired)) {
            if ($desired === $reported) {
                $desired = null;           // دستگاه فرمان را اعمال کرده؛ پاک شود
            } else {
                $response = $desired;      // هنوز اعمال نشده؛ دوباره بفرست
            }
        } else {
            $desired = null;
        }

        $entry['reported']  = $reported;
        $entry['ip']        = $ip;
        $entry['last_seen'] = time();
        $entry['desired']   = $desired;
        $all[$mac] = $entry;

        return $all;
    });

    if (!$historyOk || !$stateOk) {
        header('X-Storage-Error: ' . ($historyOk ? 'state' : 'history'));
    }

    header('X-Packet-Length: ' . strlen($raw));
    header('X-Timer-Count: ' . $p['timer_count']);

    // بدنه: فقط رشته‌ی 0/1 به تعداد رله‌ها (فرمت مورد انتظار دستگاه)
    respond(200, $response);
}

// ---------------------------------------------------------------------
// GET: تنظیم فرمان تستی و نمایش آخرین بسته
// ---------------------------------------------------------------------
function handle_admin_get()
{
    $isAdmin = isset($_GET['set']) || isset($_GET['last']);

    if ($isAdmin && ADMIN_KEY !== '' && (!isset($_GET['key']) || $_GET['key'] !== ADMIN_KEY)) {
        fail(403, 'FORBIDDEN');
    }

    if (isset($_GET['set'])) {
        $mac = isset($_GET['mac']) ? strtoupper(trim($_GET['mac'])) : '';
        $set = trim($_GET['set']);

        if (!preg_match('/^[0-9A-F]{2}(:[0-9A-F]{2}){5}$/', $mac)) {
            fail(400, 'BAD_MAC');
        }

        if ($set !== 'clear' && !preg_match('/^[01]{1,16}$/', $set)) {
            fail(400, 'BAD_STATE');
        }

        $ok = json_update(STATE_FILE, function (array $all) use ($mac, $set) {
            $entry = (isset($all[$mac]) && is_array($all[$mac])) ? $all[$mac] : array();
            $entry['desired'] = ($set === 'clear') ? null : $set;
            $all[$mac] = $entry;
            return $all;
        });

        if (!$ok) {
            fail(500, 'STORAGE_ERROR', 'FILE=state');
        }

        respond(200, 'OK;SET=' . $set . ';MAC=' . $mac);
    }

    if (isset($_GET['last'])) {
        $raw = @file_get_contents(HISTORY_FILE);
        $h   = ($raw !== false && $raw !== '') ? json_decode($raw, true) : array();

        if (!is_array($h) || count($h) === 0) {
            fail(404, 'NO_HISTORY');
        }

        http_response_code(200);
        header('Content-Type: application/json; charset=UTF-8');
        header('Cache-Control: no-store');
        echo json_encode($h[0], JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
        exit;
    }

    respond(200, 'OK;test.php;POST a binary status packet here');
}

// ---------------------------------------------------------------------
// نقطه‌ی ورود
// ---------------------------------------------------------------------
try {
    $method = isset($_SERVER['REQUEST_METHOD']) ? $_SERVER['REQUEST_METHOD'] : 'GET';

    if ($method === 'POST') {
        handle_device_post();
    } else {
        handle_admin_get();
    }
} catch (Throwable $e) {
    fail(500, 'EXCEPTION', 'MSG=' . clean_msg($e->getMessage()));
} catch (Exception $e) {
    fail(500, 'EXCEPTION', 'MSG=' . clean_msg($e->getMessage()));
}
