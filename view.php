<?php
require_once __DIR__ . '/admin_auth.php';
admin_require_login();

// view.php - Dashboard to view received packets
date_default_timezone_set('Asia/Tehran');

$storageFile = __DIR__ . '/packets_history.json';
$history = [];

$stateFile = __DIR__ . '/devices_state.json';
$deviceStates = [];

if (file_exists($stateFile)) {
    $stateData = @file_get_contents($stateFile);

    if (!empty($stateData)) {
        $deviceStates = json_decode($stateData, true) ?: [];
    }
}

if (file_exists($storageFile)) {
    $data = @file_get_contents($storageFile);
    if (!empty($data)) {
        $history = json_decode($data, true) ?: [];
    }
}

// تبدیل آرایه وضعیت کانال‌ها به بایت‌های HEX
// ترتیب بیت‌ها مطابق بسته دستگاه است: کانال اول = بیت صفر
function bits_to_hex($bits)
{
    if (!is_array($bits) || empty($bits)) {
        return '-';
    }

    $hex = '';
    $count = count($bits);

    for ($offset = 0; $offset < $count; $offset += 8) {
        $byte = 0;

        for ($bit = 0; $bit < 8 && ($offset + $bit) < $count; $bit++) {
            $byte |= (((int)$bits[$offset + $bit] & 1) << $bit);
        }

        $hex .= sprintf('%02X', $byte);
    }

    return $hex;
}
/*// اکشن پاکسازی تاریخچه
if (isset($_POST['clear_history'])) {
    @file_put_contents($storageFile, json_encode([]));
    header("Location: view.php");
    exit;
}*/
?>
<!DOCTYPE html>
<html lang="fa" dir="rtl">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
     <meta http-equiv="refresh" content="15;" >

    <title>لاگ بسته‌های دریافتی KP-4000</title>
    <style>
        * { box-sizing: border-box; font-family: Tahoma, Segoe UI, sans-serif; }
        body { background: #f0f2f5; margin: 0; padding: 20px; color: #333; }
        .container { max-width: 1200px; margin: auto; }
        .header { display: flex; justify-content: space-between; align-items: center; background: #fff; padding: 15px 25px; border-radius: 12px; box-shadow: 0 2px 8px rgba(0,0,0,0.06); margin-bottom: 20px; }
        .header h2 { margin: 0; font-size: 18px; color: #1e293b; }
        .btn { padding: 8px 16px; border: none; border-radius: 6px; cursor: pointer; text-decoration: none; font-size: 13px; font-weight: bold; }
        .btn-reload { background: #0284c7; color: white; margin-left: 8px; }
        .btn-clear { background: #ef4444; color: white; }
        .card { background: #fff; border-radius: 12px; padding: 20px; margin-bottom: 15px; box-shadow: 0 2px 8px rgba(0,0,0,0.05); }
        .card-top { display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid #e2e8f0; padding-bottom: 10px; margin-bottom: 12px; }
        .badge { background: #e0f2fe; color: #0369a1; padding: 4px 10px; border-radius: 20px; font-size: 12px; font-weight: bold; direction: ltr; }
        .badge-ip { background: #fef3c7; color: #92400e; margin-left: 8px; }
        .badge-time { background: #f1f5f9; color: #475569; }
        .io-container { display: flex; gap: 20px; flex-wrap: wrap; margin-bottom: 15px; }
        .io-box { flex: 1; min-width: 280px; background: #f8fafc; border: 1px solid #e2e8f0; padding: 12px; border-radius: 8px; }
        .io-box h4 { margin: 0 0 10px 0; font-size: 13px; color: #475569; }
        .bits-grid { display: flex; gap: 6px; direction: ltr; }
        .bit { flex: 1; text-align: center; padding: 6px 2px; border-radius: 6px; font-size: 11px; font-weight: bold; }
        .bit-on { background: #22c55e; color: white; }
        .bit-off { background: #cbd5e1; color: #475569; }
        table { width: 100%; border-collapse: collapse; margin-top: 10px; font-size: 12px; text-align: center; }
        th, td { border: 1px solid #e2e8f0; padding: 6px 8px; }
        th { background: #f1f5f9; color: #475569; }
        .hex-code { direction: ltr; font-family: monospace; font-size: 11px; background: #1e293b; color: #38bdf8; padding: 8px; border-radius: 6px; word-break: break-all; margin-top: 10px; }
        .empty { text-align: center; padding: 40px; color: #94a3b8; }
    </style>
</head>
<body>

<div class="container">
    <div class="header">
        <h2>سیستم مانیتورینگ بسته‌های KP-4000 (تعداد کل: <?= count($history) ?>)</h2>
        <div>
            <a href="view.php" class="btn btn-reload">بروزرسانی 🔄</a>
				 <form method="POST"
					  action="admin_action.php"
					  style="display:inline;"
					  onsubmit="return confirm('تاریخچه پاک شود؟');">

					<input type="hidden" name="action" value="clear_history">

					<input type="hidden"
						   name="csrf_token"
						   value="<?= htmlspecialchars(admin_csrf_token(), ENT_QUOTES, 'UTF-8') ?>">

					<button type="submit" class="btn btn-clear">
						پاک کردن لاگ
					</button>
				</form>
				<form method="POST" action="logout.php" style="display:inline;">

					<input type="hidden"
						   name="csrf_token"
						   value="<?= htmlspecialchars(admin_csrf_token(), ENT_QUOTES, 'UTF-8') ?>">

					<button type="submit" class="btn btn-reload">
						خروج مدیر
					</button>
				</form>
        </div>
    </div>
	<?php foreach ($deviceStates as $mac => $device): ?>
	<?php
			$lastSeen = isset($device['last_seen'])
				? (int)$device['last_seen']
				: 0;

			$age = $lastSeen > 0
				? max(0, time() - $lastSeen)
				: -1;

			if ($age < 0 || $age > 30) {
				$statusText = 'OFFLINE';
				$statusColor = '#ef4444';
			} elseif ($age > 20) {
				$statusText = 'UNSTABLE';
				$statusColor = '#f59e0b';
			} else {
				$statusText = 'ONLINE';
				$statusColor = '#22c55e';
			}
		?>

		<div class="card">
			<div class="card-top">
				<strong><?= htmlspecialchars($mac) ?></strong>

				<span style="
					background: <?= $statusColor ?>;
					color:white;
					padding:5px 12px;
					border-radius:20px;
					font-size:12px;">
					<?= $statusText ?>
				</span>
			</div>

			<div>
				IP:
				<?= htmlspecialchars($device['ip'] ?? '-') ?>
			</div>

			<div>
				آخرین ارتباط موفق:
				<?= $lastSeen > 0
					? date('Y-m-d H:i:s', $lastSeen)
					: '-' ?>
			</div>

			<div>
				زمان سپری‌شده:
				<?= $age >= 0 ? $age . ' ثانیه' : '-' ?>
			</div>
			<form method="POST" action="admin_action.php"
				  style="margin-top:15px;">

				<input type="hidden" name="action" value="set">

				<input type="hidden"
					   name="mac"
					   value="<?= htmlspecialchars($mac, ENT_QUOTES, 'UTF-8') ?>">

				<input type="hidden"
					   name="csrf_token"
					   value="<?= htmlspecialchars(admin_csrf_token(), ENT_QUOTES, 'UTF-8') ?>">

				<label>وضعیت درخواستی رله‌ها (از چپ به راست):</label>

				<input
					type="text"
					name="state"
					value="<?= htmlspecialchars(
						isset($device['reported']) ? $device['reported'] : '',
						ENT_QUOTES,
						'UTF-8'
					) ?>"
					pattern="[01]{1,16}"
					maxlength="16"
					required
					style="direction:ltr; width:150px; padding:7px;"
				>

				<button type="submit" class="btn btn-reload">
					ارسال فرمان
				</button>
			</form>
			<form method="POST" action="admin_action.php"
				  style="display:inline;">

				<input type="hidden" name="action" value="clear">

				<input type="hidden"
					   name="mac"
					   value="<?= htmlspecialchars($mac, ENT_QUOTES, 'UTF-8') ?>">
			<input type="hidden"
					   name="csrf_token"
					   value="<?= htmlspecialchars(admin_csrf_token(), ENT_QUOTES, 'UTF-8') ?>">

				<button type="submit" class="btn btn-clear">
					لغو فرمان معوق
				</button>
			</form>
		</div>

	<?php endforeach; ?>
    <?php if (empty($history)): ?>
			<div class="card empty">هنوز بسته‌ای دریافت نشده است.</div>
						<?php else: ?>
							<?php foreach ($history as $idx => $row): ?>
							<?php
						$relayCount = isset($row['relay_count'])
							? (int)$row['relay_count']
							: (isset($row['relays']) && is_array($row['relays'])
								? count($row['relays'])
								: 8);

						$touchCount = isset($row['touch_count'])
							? (int)$row['touch_count']
							: (isset($row['touches']) && is_array($row['touches'])
								? count($row['touches'])
								: 8);

						$relayRawHex = isset($row['relay_raw_hex'])
							? $row['relay_raw_hex']
							: bits_to_hex($row['relays'] ?? []);

						$touchRawHex = isset($row['touch_raw_hex'])
							? $row['touch_raw_hex']
							: bits_to_hex($row['touches'] ?? []);
					?>
            <div class="card">
                <div class="card-top">
                    <div>
                        <span class="badge"><?= htmlspecialchars($row['mac']) ?></span>
                        <span class="badge badge-ip"><?= htmlspecialchars($row['ip']) ?></span>
                    </div>
                    <div>
                        <span class="badge badge-time">زمان دریافت: <?= $row['received_at'] ?></span>
                    </div>
                </div>



			<!-- وضعیت رله‌ها -->
			<div class="io-box">
				<h4>
					وضعیت رله‌ها
					(بایت وضعیت: 0x<?= htmlspecialchars($relayRawHex) ?>)
				</h4>

				<div class="bits-grid">
					<?php for ($r = 0; $r < $relayCount; $r++):
						$isOn = !empty($row['relays'][$r]);
					?>
						<div class="bit <?= $isOn ? 'bit-on' : 'bit-off' ?>">
							R<?= $r + 1 ?><br>
							<?= $isOn ? 'ON' : 'OFF' ?>
						</div>
					<?php endfor; ?>
				</div>
			</div>

			<!-- وضعیت تاچ‌ها -->
			<div class="io-box">
				<h4>
					وضعیت تاچ‌ها
					(بایت وضعیت: 0x<?= htmlspecialchars($touchRawHex) ?>)
				</h4>

				<div class="bits-grid">
					<?php for ($t = 0; $t < $touchCount; $t++):
						$isTouch = !empty($row['touches'][$t]);
					?>
						<div class="bit <?= $isTouch ? 'bit-on' : 'bit-off' ?>">
							T<?= $t + 1 ?><br>
							<?= $isTouch ? '1' : '0' ?>
						</div>
					<?php endfor; ?>
				</div>
			</div>

                <!-- جدول تایمرها -->
                <?php if (!empty($row['timers'])): ?>
                    <details>
                        <summary style="cursor: pointer; font-size: 13px; font-weight: bold; color: #0284c7; margin-bottom: 8px;">
                            مشاهده جزئیات <?= count($row['timers']) ?> تایمر ارسال شده
                        </summary>
                        <table>
                            <thead>
                                <tr>
                                    <th>رله</th>
                                    <th>شناسه تایمر</th>
                                    <th>نوع (Type)</th>
                                    <th>وضعیت (Status)</th>
                                    <th>ماسک هفته</th>
                                    <th>ساعت شروع</th>
                                    <th>روشن (ثانیه)</th>
                                    <th>خاموش (ثانیه)</th>
                                    <th>تکرار</th>
                                </tr>
                            </thead>
                            <tbody>
                                <?php foreach ($row['timers'] as $tm): ?>
                                    <tr>
                                        <td>رله <?= $tm['relay_id'] ?></td>
                                        <td>تایمر <?= $tm['timer_id'] ?></td>
                                        <td><?= $tm['type'] ?></td>
                                        <td><?= $tm['status'] ?></td>
                                        <td>0x<?= sprintf('%02X', $tm['week_mask']) ?></td>
                                        <td><?= sprintf('%02d:%02d', $tm['start_hour'], $tm['start_minute']) ?></td>
                                        <td><?= number_format($tm['time_on']) ?></td>
                                        <td><?= number_format($tm['time_off']) ?></td>
                                        <td><?= $tm['count'] == 255 ? 'نامحدود' : $tm['count'] ?></td>
                                    </tr>
                                <?php endforeach; ?>
                            </tbody>
                        </table>
                    </details>
                <?php endif; ?>

                <div class="hex-code">RAW HEX: <?= chunk_split($row['raw_hex'], 2, ' ') ?></div>
            </div>
        <?php endforeach; ?>
    <?php endif; ?>
</div>

</body>
</html>
