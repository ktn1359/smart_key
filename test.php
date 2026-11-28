<?php
/*
 * test.php
 *
 * HTTP API دریافت وضعیت دستگاه‌های هوشمند KP-4000
 *
 * ============================================================
 * 1) روش ارتباط
 * ============================================================
 *
 * دستگاه اطلاعات را با HTTP POST و Content-Type زیر ارسال می‌کند:
 *
 * application/x-www-form-urlencoded
*
 * اطلاعات به صورت فیلدهای Key=Value ارسال می‌شوند.
 * مقادیر داده‌های باینری به شکل رشته HEX هستند.
 *
 * PHP این فیلدها را دریافت می‌کند، اعتبارسنجی می‌کند
 * و بسته باینری اصلی را مجدداً بازسازی می‌کند.
 *
 * سپس parse_packet() بسته بازسازی‌شده را تفسیر می‌کند.
 *
 * بنابراین HTTP POST متنی است، اما ساختار داخلی payload
 * مطابق ترتیب بایت‌های پروتکل دستگاه باقی می‌ماند.
 *
 *
 * ============================================================
 * 2) فیلدهای اصلی POST وضعیت دستگاه
 * ============================================================
 *
 * MAC:
 *   اندازه: 6 بایت = 12 کاراکتر HEX
 *   آدرس MAC دستگاه.
 *
 * IP:
 *   اندازه: 4 بایت = 8 کاراکتر HEX
 *   آدرس IPv4 دستگاه.
 *
 * DEVICE:
 *   اندازه: 1 بایت = 2 کاراکتر HEX
 *   شامل نوع دستگاه و ظرفیت رله‌ها و تاچ‌ها.
 *
 * DEVICE_INFO_2:
 *   اندازه: 1 بایت = 2 کاراکتر HEX
 *   اطلاعات وضعیت و رخدادهای تکمیلی دستگاه.
 *
 * RELAY:
 *   اندازه: متناسب با تعداد رله‌ها
 *   1 بایت برای حداکثر 8 کانال
 *   2 بایت برای 9 تا 16 کانال.
 *
 * TOUCH:
 *   اندازه: متناسب با تعداد تاچ‌ها
 *   1 بایت برای حداکثر 8 کانال
 *   2 بایت برای 9 تا 16 کانال.
 *
 * TIMER_COUNT:
 *   اندازه: عدد متنی
 *   تعداد رکوردهای تایمر ارسالی.
 *
 * TIMER0 ... TIMERn:
 *   هر رکورد: 12 بایت = 24 کاراکتر HEX
 *
 * ACKVER:
 *   اختیاری در نسخه‌های قدیمی‌تر پروتکل.
 *   در نسخه جدید به ازای هر رله یک عدد uint32
 *   با ترتیب بایت Little-Endian ارسال می‌شود.
 *
 * COUNTER:
 *   عدد متنی افزایشی برای جلوگیری از Replay.
 *
 * SIGNATURE:
 *   اندازه: 32 بایت = 64 کاراکتر HEX
 *   امضای HMAC-SHA256 بسته.
 *
 *
 * ============================================================
 * 3) ساختار داخلی payload بازسازی‌شده
 * ============================================================
 *
 * ترتیب بایت‌ها:
 *
 *   MAC             : 6 bytes
 *   IP              : 4 bytes
 *   DEVICE          : 1 byte
 *   DEVICE_INFO_2   : 1 byte
 *   RELAY           : 1 یا 2 bytes
 *   TOUCH           : 1 یا 2 bytes
 *   TIMER_COUNT     : 1 byte
 *   TIMER records   : هر رکورد 12 bytes
 *   ACKVER          : در نسخه جدید، 4 bytes به ازای هر رله
 *
 * امضای SIGNATURE به انتهای rawSigned اضافه می‌شود
 * اما جزو payload امضاشده نیست.
 *
 *
 * ============================================================
 * 4) ساختار هر رکورد تایمر: 12 بایت
 * ============================================================
 *
 * [0] timerId:
 *     بیت‌های بالا: شماره رله
 *     بیت‌های پایین: شناسه تایمر
 *
 * [1] typeStatus:
 *     نیم‌بایت پایین: نوع تایمر
 *     نیم‌بایت بالا: وضعیت تایمر
 *
 * [2] weekMask:
 *     ماسک روزهای هفته و بیت مربوط به حالت روزانه.
 *
 * [3] startHour
 * [4] startMinute
 *
 * [5..7] timeOn:
 *     عدد 24 بیتی Little-Endian، بر حسب ثانیه.
 *
 * [8..10] timeOff:
 *     عدد 24 بیتی Little-Endian، بر حسب ثانیه.
 *
 * [11] count:
 *     تعداد تکرار؛ مقدار 255 به معنی نامحدود.
 *
 *
 * ============================================================
 * 5) اعتبارسنجی امنیتی درخواست
 * ============================================================
 *
 * در نسخه جدید:
 *
 * HMAC-SHA256 با Secret اختصاصی هر دستگاه محاسبه می‌شود.
 *
 * داده امضاشده:
 *
 *   "DEVICE_TO_SERVER" + NUL
 *   + COUNTER + NUL
 *   + payload
 *
 * سپس SIGNATURE دریافتی با hash_equals مقایسه می‌شود.
 *
 * COUNTER نیز باید از آخرین Counter ثبت‌شده برای دستگاه
 * بزرگ‌تر باشد تا درخواست تکراری یا قدیمی پذیرفته نشود.
 *
 *
 * ============================================================
 * 6) پاسخ سرور به دستگاه
 * ============================================================
 *
 * در نسخه جدید دارای ACKVER:
 *
 * COUNTER=<counter>
 * &RELAY=<relay_states>
 * &CMDVER=<version_per_relay>
 * &SIGNATURE=<HMAC_SHA256>
 *
 * CMDVER شامل یک نسخه برای هر رله است.
 *
 * داده امضاشده پاسخ:
 *
 *   "SERVER_TO_DEVICE" + NUL
 *   + COUNTER + NUL
 *   + RELAY + NUL
 *   + CMDVER
 *
 * در نسخه قدیمی‌تر فاقد ACKVER، فیلد CMDVER وجود ندارد
* و قرارداد امضا نیز مطابق مسیر Legacy است.
 *
 *
 * ============================================================
 * 7) مسیرهای دیگر
 * ============================================================
 *
 * فرمان گزارش تغییر وضعیت یک رله:
 *
 *   COMMAND=RELAY
 *
 * این مسیر از handle_relay_push() پردازش می‌شود.
 *
 * تنظیم فرمان آزمایشی:
 *
 *   test.php?mac=AA:BB:CC:DD:EE:FF&set=0101
 *
 * پاک‌کردن فرمان آزمایشی:
 *
 *   test.php?mac=AA:BB:CC:DD:EE:FF&set=clear
 *
 * مشاهده آخرین بسته ذخیره‌شده:
 *
 *   test.php?last=1
 *
 * دسترسی مدیریتی:ADMIN_KEY
 * اگر ADMIN_KEY خالی باشد، کنترل کلید مدیریتی غیرفعال است.
 * بررسی و ایمن‌سازی این مسیر در بسته امنیتی بعدی انجام می‌شود.
 * ============================================================
 * نکته
 * ============================================================
 *
 * فایل باید با UTF-8 بدون BOM ذخیره شود.
 * قبل از تگ ابتدایی PHP نباید هیچ کاراکتر یا فاصله‌ای باشد.
 * تگ پایانی PHP عمداً استفاده نشده تا خروجی ناخواسته
 * وارد پاسخ HTTP نشود.
 */

// -----------------------------------------------------
// بارگذاری زیرساخت سرور 14050904
// -----------------------------------------------------

require_once __DIR__ . '/server_bootstrap.php';

require_once __DIR__ . '/http_response.php';

require_once __DIR__ . '/protocol_helpers.php';

require_once __DIR__ . '/packet_parser.php';

require_once __DIR__ . '/json_storage.php';

// ---------------------------------------------------------------------
// کمکی‌های رمزگشایی
// ---------------------------------------------------------------------

/*
 * Shadow command journal for per-relay server commands.
 * This is storage-only in this phase; the current POLL response is unchanged.
 */
function queue_relay_command_shadow(&$entry, $desired)
{
    if (!is_string($desired) || !preg_match('/^[01]{1,16}$/', $desired)) {
        return false;
    }

    $next = (isset($entry['relay_next_version']) && is_array($entry['relay_next_version']))
        ? $entry['relay_next_version']
        : array();

    $commands = (isset($entry['relay_commands']) && is_array($entry['relay_commands']))
        ? $entry['relay_commands']
        : array();

    //$now = time();

    //for ($ch = 0; $ch < strlen($desired); $ch++) {
        //$last = isset($next[$ch]) ? (int)$next[$ch] : 0;
		$now = time();

		$reported = isset($entry['reported'])
			? $entry['reported']
			: '';

		if (!is_string($reported) ||
			strlen($reported) !== strlen($desired) ||
			!preg_match('/^[01]+$/', $reported)) {
			return false;
		}

		for ($ch = 0; $ch < strlen($desired); $ch++) {

			// فقط برای رله‌ای که وضعیتش تغییر می‌کند فرمان بساز.
			if ($desired[$ch] === $reported[$ch]) {
				continue;
			}

			$last = isset($next[$ch]) ? (int)$next[$ch] : 0;	
			
        // CMDVER is planned as uint32_t on the device. Never wrap around.
        if ($last < 0 || $last >= 4294967295) {
            return false;
        }

        $version = $last + 1;
        $next[$ch] = $version;
        $commands[$ch] = array(
            'version'    => $version,
            'state'      => (int)$desired[$ch],
            'pending'    => true,
            'created_at' => $now
        );
    }

    $entry['relay_next_version'] = $next;
    $entry['relay_commands'] = $commands;

    return true;
}

function clear_relay_command_shadow(&$entry)
{
    if (!isset($entry['relay_commands']) || !is_array($entry['relay_commands'])) {
        return;
    }

    foreach ($entry['relay_commands'] as $ch => $command) {
        if (is_array($command)) {
            $entry['relay_commands'][$ch]['pending'] = false;
            $entry['relay_commands'][$ch]['cleared_at'] = time();
        }
    }
}


function handle_relay_push()
{
    $post = $_POST;

    if (!isset($post['MAC']) ||
        !preg_match('/^[0-9A-Fa-f]{12}$/', $post['MAC'])) {
        fail(400, 'BAD_MAC');
    }

    if (!isset($post['CHANNEL']) ||
        !preg_match('/^(0|[1-9][0-9]?)$/', $post['CHANNEL'])) {
        fail(400, 'BAD_CHANNEL');
    }
if (!isset($post['STATE']) ||
        !preg_match('/^[01]$/', $post['STATE'])) {
        fail(400, 'BAD_STATE');
    }

    if (!isset($post['COUNTER']) ||
        !is_valid_counter($post['COUNTER'])) {
        fail(400, 'BAD_COUNTER');
    }

    if (!isset($post['SIGNATURE']) ||
        !preg_match('/^[0-9A-Fa-f]{64}$/', $post['SIGNATURE'])) {
        fail(400, 'BAD_SIGNATURE');
    }

    $macHex = strtoupper($post['MAC']);
    $macRaw = hex2bin($macHex);
    $mac = strtoupper(
        implode(':', str_split($macHex, 2))
    );

    $channel = (int)$post['CHANNEL'];
    $state = (int)$post['STATE'];
    $counter = $post['COUNTER'];
    $signature = hex2bin($post['SIGNATURE']);

    $secrets = require BASE_DIR . '/device_secrets.php';

    if (!is_array($secrets) || !isset($secrets[$mac])) {
        fail(401, 'UNKNOWN_DEVICE');
    }

    $secretHex = $secrets[$mac];

    if (!is_string($secretHex) ||
        !preg_match('/^[0-9A-Fa-f]{64}$/', $secretHex)) {
        fail(500, 'INVALID_DEVICE_SECRET');
    }

    $secret = hex2bin($secretHex);

    // Payload مشترک بین Firmware و PHP:
    // MAC خام 6 بایت + Channel یک بایت + State یک بایت
    $payload = $macRaw . chr($channel) . chr($state);

    $signingData =
        "DEVICE_TO_SERVER" . "\0" .
        $counter . "\0" .
        $payload;

    $expectedHmac = hash_hmac(
        'sha256',
        $signingData,
        $secret,
        true
    );

    if (!hash_equals($expectedHmac, $signature)) {
        fail(401, 'INVALID_HMAC');
    }

    $response = '';
    $counterAccepted = true;
	$serverLastCounter = null; 
    $stateOk = json_update(
        STATE_FILE,
			function (array $all) use (
				$mac,
				$channel,
				$state,
				$counter,
				&$response,
				&$counterAccepted,
				&$serverLastCounter
			) {
            $entry = (isset($all[$mac]) && is_array($all[$mac]))
                ? $all[$mac]
                : array();

            // بررسی Counter تکراری یا قدیمی
				if (isset($entry['last_counter'])) {
					$lastCounter = $entry['last_counter'];
					$serverLastCounter = $lastCounter;

					if (!is_valid_counter($lastCounter) ||
						decimal_compare($counter, $lastCounter) <= 0) {
						$counterAccepted = false;
						return $all;
					}
				}

            if (!isset($entry['reported']) ||
                !is_string($entry['reported']) ||
                !preg_match('/^[01]+$/', $entry['reported'])) {
                $counterAccepted = false;
                return $all;
            }

			$reported = $entry['reported'];

			if ($channel >= strlen($reported)) {
				$counterAccepted = false;
				return $all;
			}

			// وضعیت واقعی گزارش‌شده از دستگاه
			$reported[$channel] = (string)$state;

			// اگر برای این دستگاه فرمان معوق وجود دارد،
			// تغییر محلی دستگاه باید فرمان همان کانال را خنثی کند.
			$desired = isset($entry['desired'])
				? $entry['desired']
				: null;

			if (is_string($desired) &&
				strlen($desired) === strlen($reported) &&
				preg_match('/^[01]+$/', $desired)) {

				// فقط کانالی که دستگاه تغییر داده، اصلاح می‌شود.
				$desired[$channel] = (string)$state;

				// اگر دیگر هیچ اختلافی باقی نمانده، فرمان معوق حذف شود.
				if ($desired === $reported) {
					$desired = null;
				}
			} else {
				$desired = null;
			}

			$entry['reported'] = $reported;
			$entry['desired'] = $desired;
			$entry['last_seen'] = time();
			$entry['last_counter'] = $counter;

			$response = $reported;
			$all[$mac] = $entry;

            return $all;
        }
    );

    if (!$stateOk) {
        fail(500, 'STATE_STORAGE_ERROR');
    }

		if (!$counterAccepted) {

			if ($serverLastCounter !== null) {
				fail(
					409,
					'REPLAY_OR_STALE_COUNTER',
					'LAST_COUNTER=' . $serverLastCounter
				);
			}

			fail(409, 'REPLAY_OR_STALE_COUNTER');
		}

		// Final signed response: COUNTER + RELAY + CMDVER

		$cmdverText = implode(
			',',
			array_fill(0, strlen($response), '0')
		);

		$responseSigningData =
			"SERVER_TO_DEVICE" . "\0" .
			$counter . "\0" .
			$response . "\0" .
			$cmdverText;

		$responseSignature = hash_hmac(
			'sha256',
			$responseSigningData,
			$secret,
			true
		);

		$responseBody =
			'COUNTER=' . $counter .
			'&RELAY=' . $response .
			'&CMDVER=' . $cmdverText .
			'&SIGNATURE=' . strtoupper(bin2hex($responseSignature));

		respond(200, $responseBody);
}



// ---------------------------------------------------------------------
// دریافت بسته از دستگاه
// ---------------------------------------------------------------------
function handle_device_post()
{
    $post = $_POST;
   // unset($post['TOUCH']);
	if (isset($post['COMMAND'])) {
        if ($post['COMMAND'] === 'RELAY') {
            handle_relay_push();
        }

        fail(400, 'UNKNOWN_COMMAND');
    }
    if (!is_array($post) || empty($post)) {
        fail(400, 'NO_DATA');
    }

    // خواندن و تبدیل فیلد HEX به داده باینری
    $readHex = function ($name, $byteLength) use ($post) {
        if (!isset($post[$name]) || !is_string($post[$name])) {
            fail(400, 'MISSING_FIELD', 'FIELD=' . $name);
        }

        $value = $post[$name];

        if (!preg_match('/^[0-9A-Fa-f]+$/', $value) ||
            strlen($value) !== ($byteLength * 2)) {
            fail(400, 'BAD_HEX_FIELD', 'FIELD=' . $name);
        }

        $decoded = hex2bin($value);

        if ($decoded === false) {
            fail(400, 'BAD_HEX_FIELD', 'FIELD=' . $name);
        }

        return $decoded;
    };

    // فیلدهای هویتی
    $macRaw    = $readHex('MAC', 6);
    $ipRaw     = $readHex('IP', 4);
    $deviceRaw = $readHex('DEVICE', 1);
	$deviceInfo2Raw = $readHex('DEVICE_INFO_2', 1);
	$deviceInfo2 = ord($deviceInfo2Raw[0]);

    $mac = strtoupper(
        implode(':', str_split(bin2hex($macRaw), 2))
    );

    // استخراج ظرفیت رله و تاچ از DeviceInfo
    $deviceInfo = ord($deviceRaw[0]);

    $relayCap = ($deviceInfo >> 4) & 0x03;
    $touchCap = ($deviceInfo >> 6) & 0x03;

    $relayCount = decode_channels($relayCap);
    $touchCount = decode_channels($touchCap);

    if ($relayCount === 0 || $touchCount === 0) {
        fail(400, 'UNSUPPORTED_CAPABILITY');
    }

    $relayBytes = ($relayCount + 7) >> 3;
    $touchBytes = ($touchCount + 7) >> 3;

    // وضعیت رله و تاچ
    $relayRaw = $readHex('RELAY', $relayBytes);
    $touchRaw = $readHex('TOUCH', $touchBytes);

    // تعداد تایمرها
    if (!isset($post['TIMER_COUNT']) ||
        !is_string($post['TIMER_COUNT']) ||
        !preg_match('/^(0|[1-9][0-9]{0,2})$/', $post['TIMER_COUNT'])) {
        fail(400, 'BAD_TIMER_COUNT');
    }

    $timerCount = (int)$post['TIMER_COUNT'];

    if ($timerCount > 32) {
        fail(400, 'TOO_MANY_TIMERS', 'COUNT=' . $timerCount);
    }

    // بازسازی رکوردهای تایمر
    $timersRaw = '';

    for ($i = 0; $i < $timerCount; $i++) {
        $field = 'TIMER' . $i;
        $timersRaw .= $readHex($field, 12);
    }

		// Final protocol: ACKVER is mandatory ACKVERی کردن کانتر
		if (!isset($post['ACKVER'])) {
			fail(400, 'ACKVER_REQUIRED');
		}

		$ackRaw = $readHex('ACKVER', $relayCount * 4);
		$hasAckVer = true;

    // دریافت امضای HMAC و تشخیص نسخه پروتکل
    $signature = $readHex('SIGNATURE', 32);

		// Final protocol: COUNTER is mandatory. اجباری کردن کانتر
		if (!isset($post['COUNTER']) ||
			!is_valid_counter($post['COUNTER'])) {
			fail(400, 'COUNTER_REQUIRED');
		}

		$useCounter = true;
		$counter = $post['COUNTER'];
		
    // بازسازی بسته باینری اصلی، دقیقاً با ترتیب Firmware
	$payload =
		$macRaw .
		$ipRaw .
		$deviceRaw .
		$deviceInfo2Raw .
		$relayRaw .
        $touchRaw .
		chr($timerCount) .
		$timersRaw .
		$ackRaw;

    $rawSigned = $payload . $signature;

    // خواندن Secret اختصاصی دستگاه
    $secrets = require BASE_DIR . '/device_secrets.php';

    if (!is_array($secrets) || !isset($secrets[$mac])) {
        fail(401, 'UNKNOWN_DEVICE');
    }

    $secretHex = $secrets[$mac];

    if (!is_string($secretHex) ||
        !preg_match('/^[0-9A-Fa-f]{64}$/', $secretHex)) {
        fail(500, 'INVALID_DEVICE_SECRET');
    }

    $secret = hex2bin($secretHex);

    // ساخت داده‌ای که باید امضا شود
    if ($useCounter) {
        $requestSigningData =
            "DEVICE_TO_SERVER" . "\0" .
            $counter . "\0" .
            $payload;
    } else {
        // سازگاری موقت با Firmware قدیمی
        $requestSigningData = $payload;
    }

    $expectedHmac = hash_hmac(
        'sha256',
        $requestSigningData,
        $secret,
        true
    );

    if (!hash_equals($expectedHmac, $signature)) {
        fail(401, 'INVALID_HMAC');
    }

    // تجزیه بسته باینری بازسازی‌شده
    $p = parse_packet($payload);

    $reported = implode('', $p['relays']);

    $record = array_merge(array(
        'received_at'   => date('Y-m-d H:i:s'),
        'timestamp'     => time(),
        'remote_addr'   => isset($_SERVER['REMOTE_ADDR']) ? $_SERVER['REMOTE_ADDR'] : '',
        'packet_length' => strlen($rawSigned),
        'raw_hex'       => strtoupper(bin2hex($rawSigned)),
        'relay_string'  => $reported
    ), $p);

    if ($useCounter) {
        $record['counter'] = $counter;
    }

    // به‌روزرسانی اتمیک وضعیت دستگاه و پذیرش Counter
    $mac = $p['mac'];
    $ip = $p['ip'];
	$response = $reported;
	$commandVersions = array_fill(0, $relayCount, '0');

	$ackVersions = isset($p['ack_versions'])
		? $p['ack_versions']
		: array();

	$counterAccepted = true;
	$serverLastCounter = null;
    $stateOk = json_update(
        STATE_FILE,
		function (array $all) use (
			$mac,
			$reported,
			$ip,
			$useCounter,
			$counter,
			$deviceInfo2,
			&$response,
			&$counterAccepted,
			&$serverLastCounter,
			&$commandVersions,
			$ackVersions,
			$hasAckVer,
			$relayCount
		) {
            $entry = (isset($all[$mac]) && is_array($all[$mac]))
                ? $all[$mac]
                : array();

            // جلوگیری از Counter تکراری یا قدیمی
			 if ($useCounter && isset($entry['last_counter'])) {
				$lastCounter = $entry['last_counter'];

				// نگهداری مقدار Counter سرور برای ارسال در پاسخ خطا
				$serverLastCounter = $lastCounter;

				if (!is_valid_counter($lastCounter) ||
					decimal_compare($counter, $lastCounter) <= 0) {
					$counterAccepted = false;
					return $all;
				}
			}

			$lastSeen = isset($entry['last_seen'])
				? (int)$entry['last_seen']
				: 0;

			$deviceStale =
				($lastSeen <= 0 ||
				 (time() - $lastSeen) > 20);

			$desired = isset($entry['desired'])
				? $entry['desired']
				: null;

			// رخدادهای مرتبط با قطع ارتباط یا راه‌اندازی مجدد
			// Bit 4: RESET_OCCURRED
			// Bit 5: POLL_FAILURE
			// Bit 6: WIFI_DISCONNECTED
			$recoveryEvents = ($deviceInfo2 & 0x70) !== 0;

			// در زمان بازیابی ارتباط، وضعیت واقعی دستگاه مرجع است.
			// فرمان‌های معوق قبلی سرور دیگر معتبر نیستند.
			if ($deviceStale || $recoveryEvents) {
				$desired = null;
			}

			if (!$hasAckVer) {

				// Legacy firmware: retain the old desired mechanism.
				if (is_string($desired) &&
					strlen($desired) === strlen($reported) &&
					preg_match('/^[01]+$/', $desired)) {

					if ($desired === $reported) {
						$desired = null;
					} else {
						$response = $desired;
					}
				} else {
					$desired = null;
				}

			} else {
				// New firmware uses relay_commands + CMDVER.
				$desired = null;
			}

// ACK: clear a pending command only when the device
// reports exactly the version that PHP issued.
if (!empty($ackVersions)) {

    if (!isset($entry['relay_commands']) ||
        !is_array($entry['relay_commands'])) {
        $entry['relay_commands'] = array();
    }

    for ($ch = 0; $ch < $relayCount; $ch++) {

        $ackVersion = isset($ackVersions[$ch])
            ? (string)$ackVersions[$ch]
            : '0';

        if ($ackVersion === '0')
            continue;

        if (!isset($entry['relay_commands'][$ch]) ||
            !is_array($entry['relay_commands'][$ch])) {
            continue;
        }

        $command = $entry['relay_commands'][$ch];

        if (!empty($command['pending']) &&
            isset($command['version']) &&
            (string)$command['version'] === $ackVersion) {

            $entry['relay_commands'][$ch]['pending'] = false;
            $entry['relay_commands'][$ch]['acked_at'] = time();
        }
    }
}

			// Build the response independently for every relay.بلوک پردازش فرمانهای معوق
			// A pending server command overrides the reported state.
			$desiredFromCommands = $reported;
			for ($ch = 0; $ch < $relayCount; $ch++) {

				if (!isset($entry['relay_commands'][$ch]) ||
					!is_array($entry['relay_commands'][$ch])) {
					continue;
				}

				$command = $entry['relay_commands'][$ch];

				if (empty($command['pending']))
					continue;

				if (!isset($command['version']) ||
					!isset($command['state'])) {
					continue;
				}

				$version = (string)$command['version'];
				$state = (string)$command['state'];

				if (!preg_match('/^[1-9][0-9]{0,9}$/', $version) ||
					!preg_match('/^[01]$/', $state)) {
					continue;
				}

				$response[$ch] = $state;
				$commandVersions[$ch] = $version;
				$desiredFromCommands[$ch] = $state;
			}
			$desired = ($desiredFromCommands !== $reported)
			? $desiredFromCommands
			: null;
			
            $entry['reported']  = $reported;
            $entry['ip']        = $ip;
            $entry['last_seen'] = time();
            $entry['desired']   = $desired;

            if ($useCounter) {
                $entry['last_counter'] = $counter;
            }

            $all[$mac] = $entry;

            return $all;
        }
    );

    if (!$stateOk) {
        fail(500, 'STATE_STORAGE_ERROR');
    }

		if (!$counterAccepted) {

			if ($serverLastCounter !== null) {
				fail(
					409,
					'REPLAY_OR_STALE_COUNTER',
					'LAST_COUNTER=' . $serverLastCounter
				);
			}

			fail(409, 'REPLAY_OR_STALE_COUNTER');
		}

    // ذخیره تاریخچه فقط پس از پذیرش Counter
    $historyOk = json_update(
        HISTORY_FILE,
        function (array $h) use ($record) {
            array_unshift($h, $record);
            return array_slice($h, 0, MAX_HISTORY);
        }
    );

    if (!$historyOk) {
        header('X-Storage-Error: history');
    }

    header('X-Packet-Length: ' . strlen($rawSigned));
    header('X-Timer-Count: ' . $p['timer_count']);

    // پاسخ نسخه جدید: امضای مستقل سرور
		$cmdverText = implode(',', $commandVersions);

		$responseSigningData =
			"SERVER_TO_DEVICE" . "\0" .
			$counter . "\0" .
			$response . "\0" .
			$cmdverText;

		$responseSignature = hash_hmac(
			'sha256',
			$responseSigningData,
			$secret,
			true
		);

		$responseBody =
			'COUNTER=' . $counter .
			'&RELAY=' . $response .
			'&CMDVER=' . $cmdverText .
			'&SIGNATURE=' . strtoupper(bin2hex($responseSignature));

		respond(200, $responseBody);

//     // دریافت امضای HMAC

    
//     // دریافت امضای HMAC
//     $signature = $readHex('SIGNATURE', 32);
//     //pedram
//     //$signature[0] = chr(ord($signature[0]) ^ 0x01);
    
//     // بازسازی بسته باینری اصلی، دقیقاً با ترتیب Firmware
//     $payload =
//         $macRaw .
//         $ipRaw .
//         $deviceRaw .
//         $relayRaw .
//         $touchRaw .
//         chr($timerCount) .
//         $timersRaw;

//     $rawSigned = $payload . $signature;

//     // خواندن Secret اختصاصی دستگاه
//     $secrets = require __DIR__ . '/device_secrets.php';

//     if (!isset($secrets[$mac])) {
//         fail(401, 'UNKNOWN_DEVICE');
//     }

//     $secretHex = $secrets[$mac];

//     if (!is_string($secretHex) ||
//         !preg_match('/^[0-9A-Fa-f]{64}$/', $secretHex)) {
//         fail(500, 'INVALID_DEVICE_SECRET');
//     }

//     $secret = hex2bin($secretHex);

//     // محاسبه HMAC روی بسته باینری اصلی
//     $expectedHmac = hash_hmac(
//         'sha256',
//         $payload,
//         $secret,
//         true
//     );

//     if (!hash_equals($expectedHmac, $signature)) {
//         fail(401, 'INVALID_HMAC');
//     }

//     // تجزیه بسته باینری بازسازی‌شده
//     $p = parse_packet($payload);

//     $reported = implode('', $p['relays']);
// $record = array_merge(array(
//         'received_at'   => date('Y-m-d H:i:s'),
//         'timestamp'     => time(),
//         'remote_addr'   => isset($_SERVER['REMOTE_ADDR']) ? $_SERVER['REMOTE_ADDR'] : '',
//         'packet_length' => strlen($rawSigned),
//         'raw_hex'       => strtoupper(bin2hex($rawSigned)),
//         'relay_string'  => $reported
//     ), $p);

//     // ذخیره تاریخچه
//     $historyOk = json_update(HISTORY_FILE, function (array $h) use ($record) {
//         array_unshift($h, $record);
//         return array_slice($h, 0, MAX_HISTORY);
//     });

//     // ذخیره وضعیت دستگاه و بررسی فرمان در انتظار
//     $mac      = $p['mac'];
//     $response = $reported;
//     $ip       = $p['ip'];

//     $stateOk = json_update(STATE_FILE, function (array $all) use ($mac, $reported, $ip, &$response) {
//         $entry = (isset($all[$mac]) && is_array($all[$mac]))
//             ? $all[$mac]
//             : array();

//         $desired = isset($entry['desired']) ? $entry['desired'] : null;

//         if (is_string($desired) &&
//             strlen($desired) === strlen($reported) &&
//             preg_match('/^[01]+$/', $desired)) {

//             if ($desired === $reported) {
//                 $desired = null;
//             } else {
//                 $response = $desired;
//             }
//         } else {
//             $desired = null;
//         }

//         $entry['reported']  = $reported;
//         $entry['ip']        = $ip;
//         $entry['last_seen'] = time();
//         $entry['desired']   = $desired;

//         $all[$mac] = $entry;

//         return $all;
//     });

//     if (!$historyOk || !$stateOk) {
//         header('X-Storage-Error: ' . ($historyOk ? 'state' : 'history'));
//     }

//     header('X-Packet-Length: ' . strlen($rawSigned));
//     header('X-Timer-Count: ' . $p['timer_count']);

//     // پاسخ مورد انتظار Firmware
//     respond(200, $response);
}

// ---------------------------------------------------------------------
// GET: تنظیم فرمان تستی و نمایش آخرین بسته
// ---------------------------------------------------------------------
function handle_admin_get()
{
    // مسیرهای قدیمی مدیریت دیگر مجاز نیستند.
    if (isset($_GET['set']) || isset($_GET['last'])) {
        fail(403, 'ADMIN_ROUTE_DISABLED');
    }

    respond(
        200,
        'OK;test.php;POST a signed device packet here'
    );
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

    error_log(
        '[UNCAUGHT EXCEPTION] Class=' . get_class($e) .
        ' Message=' . $e->getMessage() .
        ' File=' . $e->getFile() .
        ' Line=' . $e->getLine()
    );

    fail(500, 'EXCEPTION');
}