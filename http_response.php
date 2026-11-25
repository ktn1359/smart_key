<?php
function clean_msg($s)
{
    return substr(
        preg_replace(
            '/[^\x20-\x7E]/',
            '?',
            str_replace(
                array(';', "\r", "\n"),
                ' ',
                (string)$s
            )
        ),
        0,
        120
    );
}

function respond($code, $body)
{
    while (ob_get_level() > 0) {
        ob_end_clean();
    }

    http_response_code($code);

    header('Content-Type: text/plain; charset=UTF-8');
    header('Cache-Control: no-store');
    header('Content-Length: ' . strlen($body));

    echo $body;
    exit;
}

function fail($code, $name, $extra = '')
{
    respond(
        $code,
        'ERROR;CODE=' . $name .
        ($extra !== '' ? ';' . $extra : '')
    );
}