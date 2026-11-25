<?php

// -----------------------------------------------------
// ذخیره‌سازی امن JSON
// دارای Lock، فایل موقت و جایگزینی Atomic
// -----------------------------------------------------

function json_update($file, $callback)
{
    $lockPath = $file . '.lock';
    $lock = @fopen($lockPath, 'c');

    if (!$lock) {
        error_log(
            'json_update: cannot open lock file: ' . $lockPath
        );
        return false;
    }

    if (!flock($lock, LOCK_EX)) {
        fclose($lock);

        error_log(
            'json_update: cannot lock: ' . $lockPath
        );

        return false;
    }

    $tmp = null;
    $out = null;
    $result = false;

    try {

        if (!is_file($file) || !is_readable($file)) {
            throw new RuntimeException(
                'JSON file missing or unreadable'
            );
        }

        $raw = @file_get_contents($file);

        if ($raw === false || trim($raw) === '') {
            throw new RuntimeException(
                'JSON file is empty or unreadable'
            );
        }

        $data = json_decode($raw, true);

        if (
            json_last_error() !== JSON_ERROR_NONE ||
            !is_array($data)
        ) {
            throw new RuntimeException(
                'Invalid JSON: ' . json_last_error_msg()
            );
        }

        $updated = $callback($data);

        if (!is_array($updated)) {
            throw new RuntimeException(
                'Callback did not return an array'
            );
        }

        $flags = JSON_UNESCAPED_UNICODE |
                 JSON_UNESCAPED_SLASHES;

        $oldJson = json_encode($data, $flags);
        $newJson = json_encode($updated, $flags);

        if ($oldJson === false || $newJson === false) {
            throw new RuntimeException(
                'JSON encoding failed'
            );
        }

        // عدم بازنویسی در صورت نبود تغییر
        if ($oldJson === $newJson) {

            $result = true;

        } else {

            // ایجاد فایل موقت در همان پوشه
            $tmp = @tempnam(
                dirname($file),
                basename($file) . '.tmp.'
            );

            if ($tmp === false) {
                throw new RuntimeException(
                    'Cannot create temporary file'
                );
            }

            $out = @fopen($tmp, 'wb');

            if (!$out) {
                throw new RuntimeException(
                    'Cannot open temporary file'
                );
            }

            $length = strlen($newJson);
            $offset = 0;

            while ($offset < $length) {

                $written = fwrite(
                    $out,
                    substr($newJson, $offset)
                );

                if ($written === false || $written === 0) {
                    throw new RuntimeException(
                        'Incomplete file write'
                    );
                }

                $offset += $written;
            }

            if (!fflush($out)) {
                throw new RuntimeException(
                    'File flush failed'
                );
            }

            if (!fclose($out)) {
                $out = null;
throw new RuntimeException(
                    'File close failed'
                );
            }

            $out = null;

            // حفظ مجوزهای فایل قبلی
            $permissions = @fileperms($file);

            if ($permissions !== false) {
                @chmod($tmp, $permissions & 0777);
            }

            // جایگزینی اتمیک
            if (!@rename($tmp, $file)) {
                throw new RuntimeException(
                    'Atomic file replacement failed'
                );
            }

            $tmp = null;
            $result = true;
        }

    } catch (Throwable $e) {

        error_log(
            'json_update failed: ' .
            $e->getMessage() .
            ' File=' . $file
        );

        $result = false;

    } finally {

        if (is_resource($out)) {
            fclose($out);
        }

        if ($tmp !== null && is_file($tmp)) {
            @unlink($tmp);
        }

        flock($lock, LOCK_UN);
        fclose($lock);
    }

    return $result;
}