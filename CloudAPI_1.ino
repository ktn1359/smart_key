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

#define CLOUD_CONFIG_FILE "/cloud.dat"

struct CloudConfig
{
    bool     enabled;
    char     host[64];
    uint16_t port;
    char     deviceId[24];
    char     apiKey[40];
    uint32_t pollIntervalMs;
};
CloudConfig cloudConfig;

unsigned long cloudLastPoll = 0;

//------------------------------------------------------
//                  STORAGE
//------------------------------------------------------

bool loadCloudConfig()
{
    if (!LittleFS.exists(CLOUD_CONFIG_FILE))
        return false;

    File f = LittleFS.open(CLOUD_CONFIG_FILE, "r");

    if (!f)
        return false;

    if (f.size() != sizeof(cloudConfig))
    {
        f.close();
        return false;
    }

    size_t readSize = f.read((uint8_t*)&cloudConfig, sizeof(cloudConfig));
    f.close();

    return readSize == sizeof(cloudConfig);
}

void saveCloudConfig()
{
    File f = LittleFS.open(CLOUD_CONFIG_FILE, "w");

    if (!f)
    {
        if (debug)
            DBG_PRINTLN("Cloud: save failed");
        return;
    }

    f.write((uint8_t*)&cloudConfig, sizeof(cloudConfig));
    f.close();
}

//------------------------------------------------------
//                  INIT  (از startSystem() صدا زده میشه)
//------------------------------------------------------

// MAC رو بدون ':' برمی‌گردونه، مثلاً "A1B2C3D4E5F6" — یکتا و ثابت روی هر ماژول،
// نیازی به تخصیص دستی نیست و با تعویض IP یا شبکه هم عوض نمی‌شه.
String getDeviceUID()
{
    String mac = WiFi.macAddress();   // "A1:B2:C3:D4:E5:F6"
    mac.replace(":", "");
    return mac;
}

void initCloudAPI()
{
    bool loaded = loadCloudConfig();

    if (!loaded)
    {
        memset(&cloudConfig, 0, sizeof(cloudConfig));

        cloudConfig.enabled = false;
        cloudConfig.port = 80;
        cloudConfig.pollIntervalMs = 5000;
    }

    // اگه deviceId خالیه (چه چون فایل نبود، چه چون از نسخه‌ی قبلی مونده)،
    // همیشه از MAC پر می‌کنیم — همین باعث میشه شناسه یکتا و بدون دخالت کاربر باشه.
    if (strlen(cloudConfig.deviceId) == 0)
        strncpy(cloudConfig.deviceId, getDeviceUID().c_str(), sizeof(cloudConfig.deviceId) - 1);

    if (!loaded)
    {
        saveCloudConfig();

        if (debug)
            DBG_PRINTLN("Cloud: default config created (disabled)");
    }
}

//------------------------------------------------------
//                  APPLY INCOMING COMMAND
//------------------------------------------------------

void applyCloudRelayCommand(const String &body)
{
    int idx = 0;
    int start = 0;

    for (int i = 0; i <= (int)body.length() && idx < TOUCH_COUNT; i++)
    {
        if (i == (int)body.length() || body[i] == ',')
        {
            String token = body.substring(start, i);
            token.trim();

            if (token == "1")
                relayOn(idx, false);
            else if (token == "0")
                relayOff(idx, false);
            // هر مقدار دیگه (مثلاً x) یعنی این رله رو دست نزن

            start = i + 1;
            idx++;
        }
    }
}

//------------------------------------------------------
//                  POLL COMMANDS FROM SERVER
//------------------------------------------------------

void cloudPollCommands()
{
    WiFiClient client;
    HTTPClient http;

    String url = "http://";
    url += cloudConfig.host;
    url += ":";
    url += String(cloudConfig.port);
    url += "/api/device/";
    url += cloudConfig.deviceId;
    url += "/poll?key=";
    url += cloudConfig.apiKey;

    if (!http.begin(client, url))
    {
        if (debug)
            DBG_PRINTLN("Cloud: poll begin failed");
        return;
    }

    http.setTimeout(4000);
    int code = http.GET();

    if (code == 200)
    {
        String body = http.getString();
        body.trim();

        if (body.length() > 0 && body != "NOCHANGE")
            applyCloudRelayCommand(body);
    }
    else if (debug)
    {
        DBG_PRINT("Cloud: poll HTTP code=");
        DBG_PRINTLN(code);
    }

    http.end();
}

//------------------------------------------------------
//                  PUSH STATUS TO SERVER
//------------------------------------------------------

void cloudPushStatus()
{
    WiFiClient client;
    HTTPClient http;

    String url = "http://";
    url += cloudConfig.host;
    url += ":";
    url += String(cloudConfig.port);
    url += "/api/device/";
    url += cloudConfig.deviceId;
    url += "/status?key=";
    url += cloudConfig.apiKey;

    if (!http.begin(client, url))
        return;

    String json = "{\"relay\":[";

    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        json += relayState[i] ? "1" : "0";

        if (i < TOUCH_COUNT - 1)
            json += ",";
    }

    json += "],\"rssi\":";
    json += String(WiFi.RSSI());
    json += ",\"ip\":\"";
    json += WiFi.localIP().toString();
    json += "\"}";

    http.addHeader("Content-Type", "application/json");
    http.setTimeout(4000);

    int code = http.POST(json);

    if (debug)
    {
        DBG_PRINT("Cloud: status push code=");
        DBG_PRINTLN(code);
    }

    http.end();
}

//------------------------------------------------------
//                  TASK  (از runSystem() صدا زده میشه)
//------------------------------------------------------

void taskCloudSync()
{
    if (!cloudConfig.enabled)
        return;

    if (!wifiConnected)
        return;

    if (strlen(cloudConfig.host) == 0)
        return;

    unsigned long now = millis();

    if (now - cloudLastPoll < cloudConfig.pollIntervalMs)
        return;

    cloudLastPoll = now;

    cloudPollCommands();
    cloudPushStatus();
}

//------------------------------------------------------
//                  WEB CONFIG ENDPOINTS
//                  (اختیاری؛ برای تنظیم از طریق /wifi UI یا curl)
//------------------------------------------------------

void handleGetCloud()
{
    String json = "{";

    json += "\"enabled\":";
    json += cloudConfig.enabled ? "true" : "false";
    json += ",\"host\":\"";
    json += cloudConfig.host;
    json += "\",\"port\":";
    json += String(cloudConfig.port);
    json += ",\"deviceId\":\"";
    json += cloudConfig.deviceId;
    json += "\",\"pollIntervalMs\":";
    json += String(cloudConfig.pollIntervalMs);
    json += "}";

    server.send(200, "application/json", json);
}

void handleSetCloud()
{
    if (server.hasArg("host"))
        strncpy(cloudConfig.host, server.arg("host").c_str(), sizeof(cloudConfig.host) - 1);

    if (server.hasArg("port"))
        cloudConfig.port = (uint16_t)server.arg("port").toInt();

    if (server.hasArg("deviceId"))
        strncpy(cloudConfig.deviceId, server.arg("deviceId").c_str(), sizeof(cloudConfig.deviceId) - 1);

    if (server.hasArg("apiKey"))
        strncpy(cloudConfig.apiKey, server.arg("apiKey").c_str(), sizeof(cloudConfig.apiKey) - 1);

    if (server.hasArg("interval"))
        cloudConfig.pollIntervalMs = (uint32_t)server.arg("interval").toInt();

    if (server.hasArg("enabled"))
        cloudConfig.enabled = server.arg("enabled").toInt() != 0;

    saveCloudConfig();

    server.send(200, "text/plain", "OK");
}
