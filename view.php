<?php
// view.php - Dashboard to view received packets
date_default_timezone_set('Asia/Tehran');

$storageFile = __DIR__ . '/packets_history.json';
$history = [];

if (file_exists($storageFile)) {
    $data = @file_get_contents($storageFile);
    if (!empty($data)) {
        $history = json_decode($data, true) ?: [];
    }
}

// اکشن پاکسازی تاریخچه
if (isset($_POST['clear_history'])) {
    @file_put_contents($storageFile, json_encode([]));
    header("Location: view.php");
    exit;
}
?>
<!DOCTYPE html>
<html lang="fa" dir="rtl">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
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
            <form method="POST" style="display:inline;" onsubmit="return confirm('تاریخچه پاک شود؟');">
                <button type="submit" name="clear_history" value="1" class="btn btn-clear">پاک کردن لاگ</button>
            </form>
        </div>
    </div>

    <?php if (empty($history)): ?>
        <div class="card empty">هنوز بسته‌ای دریافت نشده است.</div>
    <?php else: ?>
        <?php foreach ($history as $idx => $row): ?>
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

                <div class="io-container">
                    <!-- وضعیت رله‌ها -->
                    <div class="io-box">
                        <h4>وضعیت رله‌ها (بایت وضعیت: 0x<?= sprintf('%02X', $row['relay_byte']) ?>)</h4>
                        <div class="bits-grid">
                            <?php for ($r = 0; $r < 8; $r++): 
                                $isOn = !empty($row['relays'][$r]);
                            ?>
                                <div class="bit <?= $isOn ? 'bit-on' : 'bit-off' ?>">
                                    R<?= $r + 1 ?><br><?= $isOn ? 'ON' : 'OFF' ?>
                                </div>
                            <?php endfor; ?>
                        </div>
                    </div>

                    <!-- وضعیت تاچ‌ها -->
                    <div class="io-box">
                        <h4>وضعیت تاچ‌ها (بایت وضعیت: 0x<?= sprintf('%02X', $row['touch_byte']) ?>)</h4>
                        <div class="bits-grid">
                            <?php for ($t = 0; $t < 8; $t++): 
                                $isTouch = !empty($row['touches'][$t]);
                            ?>
                                <div class="bit <?= $isTouch ? 'bit-on' : 'bit-off' ?>">
                                    T<?= $t + 1 ?><br><?= $isTouch ? '1' : '0' ?>
                                </div>
                            <?php endfor; ?>
                        </div>
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
