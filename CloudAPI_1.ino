//======================================================
//        CLOUD / CUSTOM SERVER SYNC (outbound HTTP)
//======================================================


//   1) GET  http://HOST:PORT/api/device/{deviceId}/poll?key=API_KEY
//      پاسخ متنی ساده (نه JSON)، یکی از این دو حالت:
//        - "NOCHANGE"                → فرمانی نیست
//        - "1,0,x,1"                 → وضعیت هدف هر رله؛ 0/1 = تغییر بده،
//                                        هر کاراکتر دیگه (مثلاً x) = دست نزن
//                                      تعداد آیتم‌ها باید برابر TOUCH_COUNT باشه (فعلاً 4)
//
//   2) POST http://HOST:PORT/api/device/{deviceId}/status?key=API_KEY
//      بدنه: JSON وضعیت فعلی دستگاه، مثلاً:
//        {"relay":[1,0,0,1],"rssi":-61,"ip":"192.168.1.23"}
//      سرور فقط اینو لاگ/ذخیره می‌کنه؛ پاسخش برای دستگاه مهم نیست.
//
// این پروتکل عمداً ساده نگه داشته شده (بدون JSON parsing روی دستگاه) چون
// حافظه‌ی ESP8266 محدوده. اگه بعداً خواستی، می‌تونی بدنه‌ی poll رو JSON کنی
// و از کتابخانه‌ی ArduinoJson استفاده کنی.

#if MODULE_TYPE == MODULE_ESP12F
    #include <ESP8266HTTPClient.h>
#elif MODULE_TYPE == MODULE_ESP32C3
    #include <HTTPClient.h>
#endif

//======================================================
// HELPSCHOOL HTTP API
//======================================================

const char* CLOUD_STATUS_URL =
    "http://helpschool.ir/status/";

const char* CLOUD_SET_ON_URL =
    "http://helpschool.ir/seton/?ch=";

const char* CLOUD_SET_OFF_URL =
    "http://helpschool.ir/setoff/?ch=";

const unsigned long CLOUD_POLL_INTERVAL_MS = 10000UL;
const unsigned long CLOUD_HTTP_TIMEOUT_MS = 3000UL;
const unsigned long CLOUD_RETRY_INTERVAL_MS = 1000UL;

unsigned long cloudLastPoll = 0;
unsigned long cloudLastPushAttempt = 0;

bool cloudFirstPoll = true;
bool cloudSynced = false;

// آخرین وضعیت شناخته‌شده سمت سرور
bool cloudKnownRelayState[TOUCH_COUNT] = {false};

//======================================================
// HTTP GET
//======================================================

int cloudHttpGet(const String& url, String* response = nullptr)
{
    WiFiClient client;
    HTTPClient http;

    http.setTimeout(CLOUD_HTTP_TIMEOUT_MS);

    if (!http.begin(client, url))
    {
        DBG_PRINTLN("[Cloud] HTTP begin failed");
        return -1;
    }

    int code = http.GET();

    if (code >= 200 && code < 300 && response != nullptr)
        *response = http.getString();

    http.end();

    return code;
}

//======================================================
// READ SERVER STATUS
//======================================================

bool cloudPollStatus()
{
    String response;

    int code = cloudHttpGet(CLOUD_STATUS_URL, &response);

    if (code != 200)
    {
        DBG_PRINTF("[Cloud] Status HTTP error: %d\n", code);
        return false;
    }

    response.trim();

    // فقط پاسخ دقیق چهاررقمی معتبر است
    if (response.length() != TOUCH_COUNT)
    {
        DBG_PRINTLN("[Cloud] Invalid status length");
        return false;
    }

    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        if (response[i] != '0' && response[i] != '1')
        {
            DBG_PRINTLN("[Cloud] Invalid status content");
            return false;
        }
    }

    DBG_PRINT("[Cloud] Server status: ");
    DBG_PRINTLN(response);

    // وضعیت دریافتی از سرور را روی رله‌ها اعمال کن
    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        bool targetState = (response[i] == '1');

        if (relayState[i] != targetState)
        {
            if (targetState)
                manualRelayOn(i);
            else
                manualRelayOff(i);
        }

        cloudKnownRelayState[i] = relayState[i];
    }

    cloudSynced = true;

    return true;
}

//======================================================
// SEND ONE RELAY CHANGE TO SERVER
//======================================================

bool cloudPushRelayState(byte channel, bool state)
{
    if (channel >= TOUCH_COUNT)
        return false;

    String url = state ? CLOUD_SET_ON_URL : CLOUD_SET_OFF_URL;
    url += String(channel);

    int code = cloudHttpGet(url);

    if (code >= 200 && code < 300)
    {
        DBG_PRINT("[Cloud] Relay ");
        DBG_PRINT(channel);
        DBG_PRINT(" set ");
        DBG_PRINTLN(state ? "ON" : "OFF");
return true;
    }

    DBG_PRINT("[Cloud] Relay command failed, HTTP=");
    DBG_PRINTLN(code);

    return false;
}

//======================================================
// MAIN CLOUD TASK
// Call from runSystem()
//======================================================

void initCloudAPI()
{
    cloudLastPoll = 0;
    cloudLastPushAttempt = 0;

    cloudFirstPoll = true;
    cloudSynced = false;

    for (byte i = 0; i < TOUCH_COUNT; i++)
        cloudKnownRelayState[i] = false;

    DBG_PRINTLN("[Cloud] HTTP API initialized");
}

void taskCloudSync()
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    unsigned long now = millis();

    // Poll immediately after startup, then every 10 seconds
    if (cloudFirstPoll ||
        (unsigned long)(now - cloudLastPoll) >=
        CLOUD_POLL_INTERVAL_MS)
    {
        cloudLastPoll = now;
        cloudFirstPoll = false;

        cloudPollStatus();
    }

    // Until the first valid server status is received,
    // do not push local relay states to the server.
    if (!cloudSynced)
        return;

    // Rate-limit retries when the network/server is unavailable
    if ((unsigned long)(now - cloudLastPushAttempt) <
        CLOUD_RETRY_INTERVAL_MS)
        return;

    // Send at most one changed relay per retry interval
    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        if (relayState[i] != cloudKnownRelayState[i])
        {
            cloudLastPushAttempt = now;

            if (cloudPushRelayState(i, relayState[i]))
                cloudKnownRelayState[i] = relayState[i];

            break;
        }
    }
}
