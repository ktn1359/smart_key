//14050820 1800   

//======================================================
//                  MODULE TYPE// کدام ماژول روی برد است ؟
//======================================================

#define MODULE_ESP12F    1
#define MODULE_ESP32C3   2

#define MODULE_TYPE      MODULE_ESP12F   // روز مهاجرت، فقط همین خط رو به MODULE_ESP32C3 عوض کنم

#define TOUCH_COUNT 4 // تعداد رله و تاچ‌های مدل فعلی؛ برای مدل‌های بعدی 8 یا 16

#ifndef DEVICE_TYPE_CODE
#define DEVICE_TYPE_CODE 1 // کد قابل تنظیم نوع محصول؛ 0 رزرو است
#endif


#define PERSIST_VERSION   1
#define PERSIST_FILE "/runtime.dat"


//========================//1==============================
//                      INCLUDE
//======================================================
#include "TimerForward.h"  // Timer must be declared before Arduino-generated prototypes

#if MODULE_TYPE == MODULE_ESP12F

    #include <ESP8266WiFi.h>
    #include <ESP8266WebServer.h>
    #include <ESP8266mDNS.h>
    #include <coredecls.h>      // settimeofday_cb()
    #include <lwip/apps/sntp.h> // sntp_stop()
    #include <ESPAsyncTCP.h>
    typedef ESP8266WebServer WebServerType;

#elif MODULE_TYPE == MODULE_ESP32C3

    #include <WiFi.h>
    #include <WebServer.h>
    #include <ESPmDNS.h>
    #include <esp_sntp.h>       // sntp_set_time_sync_notification_cb()
    #include <AsyncTCP.h>
    typedef WebServer WebServerType;

#endif

#include <WiFiUdp.h>
#include <LittleFS.h>
#include <Wire.h>
#include <time.h>




//======================================================
//                  SYSTEM STATE
//======================================================

struct SystemState
{
    // Boot
    uint32_t bootCounter;
    uint32_t restartCounter;
    uint32_t crashCounter;
    // WiFi
    bool wifiConnected;
    bool apEnabled;
    bool provisioningRunning;

    byte currentWiFi;
    byte retryCount;

    // Network
    bool mdnsStarted;
    bool udpStarted;

    // Device
    bool relayNeedSave;

    // Health
    uint32_t uptime;
    uint32_t freeHeap;
    int16_t rssi;

    // Service
    bool safeMode;
};
SystemState sys;
//======================================================
//                  BOOT INFO
//======================================================

struct BootInfo
{
    uint32_t bootCounter;

    uint32_t restartCounter;

    uint32_t crashCounter;

    uint32_t lastBootMillis;

    uint8_t lastResetReason;

    bool safeMode;
};
BootInfo boot;

//======================================================
//                  HEALTH INFO
//======================================================

struct HealthInfo
{
    uint32_t uptime; 
    uint32_t freeHeap; 
    uint32_t maxFreeBlock; 
    int16_t rssi; 
    bool wifiConnected; 
    bool apRunning; 
    bool mqttConnected; 
    bool localClientConnected; 
    uint32_t lastHealthUpdate;
    bool updated;

};
HealthInfo health;

//======================================================
//              PERSISTENT RUNTIME DATA
//======================================================

struct PersistentData
{
    uint32_t bootCounter;

    uint32_t restartCounter;

    uint32_t crashCounter;

    uint8_t lastResetReason;

    uint32_t lastBootUnix;

    uint8_t lastConnectedWiFi;

    uint32_t dataVersion;
};
PersistentData persist;

//======================================================
//              TIME Manager & Scheduler 
//======================================================
#define MAX_TIMERS (TOUCH_COUNT * 2) // دو تایمر برای هر رله
#define TIME_RESYNC_INTERVAL 21600000UL   // 6 hours
#define TIME_STALE_THRESHOLD 86400UL   // 24 ساعت - فقط برای گزارش کیفیت Sync و اولویت NTP نسبت به Client؛ مانع اجرای Scheduler نیست
#define CLOCK_JUMP_THRESHOLD_SEC 120   // جهش ساعت بیشتر از این مقدار (ثانیه) → Catch-up دوباره اجرا می‌شود

#define TIMER_MODE_NORMAL   0
#define TIMER_MODE_BLINKER  1

#define BLINK_MIN_SECONDS   5UL     // Legacy UI minimum
#define BLINK_MAX_SECONDS   43200UL // Legacy UI maximum (12 hours)

// configTime() در این فریمور سه آدرس سرور دریافت می‌کند؛
// هر سه عضو باید واقعاً در آرایه موجود باشند.
const char* NTP_SERVERS[] =
{
    // "time.ir",
    "ntp.time.ir",
    "pool.ntp.org"
};

// اگر ساعت هنوز از NTP معتبر نشده باشد، تلاش مجدد سریع‌تر انجام می‌شود.
// پس از Sync موفق، فاصله‌ی Syncهای دوره‌ای همان ۶ ساعت باقی می‌ماند.
#define TIME_NTP_RETRY_INTERVAL 60000UL  // 60 seconds

enum TimeQuality
{
    TIME_UNAVAILABLE = 0,
    TIME_ESTIMATED,
    TIME_SYNCED
};
enum TimeSource
{
    SOURCE_NONE = 0,   // هنوز هیچ منبع معتبری زمان را تنظیم نکرده (فقط ساعت داخلی)
    SOURCE_CLIENT,     // آخرین اصلاح از طریق Web Client / App بوده
    SOURCE_NTP         // آخرین اصلاح از طریق NTP واقعی بوده
};
enum TimerType : uint8_t
{
    TIMER_ONCE = 0,
    TIMER_DAILY,
    TIMER_WEEKLY
    // بعداً: TIMER_MONTHLY, TIMER_SUNRISE, TIMER_SUNSET, ...
};

// Compact Timer protocol: exact 12-byte configuration record.
// Kept separate from legacy runtime fields during migration.
enum TimerProtocolValue : uint8_t {
    TIMER_PROTOCOL_DISABLED = 0,
    TIMER_PROTOCOL_SCHEDULER = 1,
    TIMER_PROTOCOL_BLINKER = 2,
    TIMER_PROTOCOL_RESERVED_3 = 3,
    TIMER_STATUS_QUEUED = 4,
    TIMER_STATUS_RUNNING = 5,
    TIMER_STATUS_RESERVED = 6,
    TIMER_STATUS_CANCELLED = 7
};

struct __attribute__((packed)) TimerConfigPacket {
    uint8_t timerId;       // high nibble relay index 0..15, low nibble timer index 0..1
    uint8_t typeStatus;    // low nibble: type 0..3; high nibble: runtime status 0,4..7
    uint8_t weekMask;
    uint8_t startHour;
    uint8_t startMinute;
    uint8_t timeOn[3];     // uint24 seconds, little-endian
    uint8_t timeOff[3];    // uint24 seconds, little-endian
    uint8_t count;         // 255 = infinite Blinker repetition
};
static_assert(sizeof(TimerConfigPacket) == 12, "TimerConfigPacket must be exactly 12 bytes");

static uint32_t timerReadU24LE(const uint8_t value[3]) {
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) | ((uint32_t)value[2] << 16);
}
static void timerWriteU24LE(uint8_t value[3], uint32_t seconds) {
    value[0] = (uint8_t)(seconds & 0xFF);
    value[1] = (uint8_t)((seconds >> 8) & 0xFF);
    value[2] = (uint8_t)((seconds >> 16) & 0xFF);
}
static uint32_t timerBlinkFiniteDurationSeconds(uint32_t onSeconds, uint32_t offSeconds, uint16_t count) {
    // count is the number of ON periods. There is no OFF period after the final ON.
    if (count == 0 || count == 255) return 0; // 255 = infinite
    return (uint32_t)count * onSeconds + (uint32_t)(count - 1) * offSeconds;
}

static bool timerProtocolConfigValid(const TimerConfigPacket &packet) {
    if (packet.startHour > 23 || packet.startMinute > 59) return false;
    if ((packet.timerId >> 4) > 15 || (packet.timerId & 0x0F) > 1) return false;
    // Low nibble is the configured type; high nibble is runtime status.
    const uint8_t timerType = packet.typeStatus & 0x0F;
    const uint8_t runtimeStatus = packet.typeStatus >> 4;
    if (timerType > TIMER_PROTOCOL_RESERVED_3) return false;
    if (runtimeStatus != 0 && runtimeStatus < TIMER_STATUS_QUEUED) return false;
    if (runtimeStatus > TIMER_STATUS_CANCELLED) return false;
    const uint32_t onSeconds = timerReadU24LE(packet.timeOn);
    const uint32_t offSeconds = timerReadU24LE(packet.timeOff);
    if (timerType == TIMER_PROTOCOL_DISABLED) return true;
    if (timerType == TIMER_PROTOCOL_SCHEDULER) return true;
    if (timerType == TIMER_PROTOCOL_BLINKER)
        return onSeconds > 0 && offSeconds > 0 && packet.count > 0;
    // Type 3 is reserved and not accepted until defined.
    return false;
}

struct TimeManager
{
    bool initialized;
    bool ntpSynced;

    TimeQuality quality;
    TimeSource  source;      // <-- این خط اضافه شد

    unsigned long lastSyncMillis;
    unsigned long lastTimeCheckMillis;

    time_t lastSyncTime;
};
TimeManager timeManager;

/*struct Timer
{
    // timerId: high nibble = relay index (0..15), low nibble = timer index (0..1)
    uint8_t   timerId;
    bool      enabled;
    TimerType type;

    uint8_t hour;
    uint8_t minute;

    uint8_t weekMask;       // بیت 0=یکشنبه ... بیت 6=شنبه (فقط WEEKLY)

    uint8_t relayIndex;
    bool    relayState;     // وضعیت شروع بازه/اکشن

    uint32_t durationSeconds;   // 0 = بدون بازه (اکشن لحظه‌ای تنها)
    time_t   activeUntil;       // فقط runtime؛ لحظه‌ی پایان بازه‌ی فعال فعلی

    uint32_t lastTriggerMinute;
};*/
struct Timer
{
    // timerId: high nibble = relay index (0..15), low nibble = timer index (0..1)
    uint8_t   timerId;
    bool      enabled;
    TimerType type;

    uint8_t year;       // YY: 0..99  → 2000..2099
    uint8_t month;      // 1..12
    uint8_t day;        // 1..31

    uint8_t hour;       // 0..23
    uint8_t minute;     // 0..59
    uint8_t second;     // 0..59

    uint8_t weekMask;

    uint8_t relayIndex;
    bool    relayState;

    uint32_t durationSeconds;
    time_t   activeUntil;

    // uint32_t lastTriggerMinute;
    uint32_t lastTriggerSecond;
    uint32_t cancelledOccurrenceSecond;

    // =========================
    // BLINKER CONFIGURATION
    // =========================
    uint8_t  mode;

    uint32_t blinkOnSeconds;
    uint32_t blinkOffSeconds;
    uint16_t blinkerCount;

    // =========================
    // BLINKER RUNTIME
    // =========================
    time_t   blinkStartTime;
    time_t   blinkEndTime;
    time_t   blinkNextToggle;
    bool     blinkState;
    uint16_t blinkerExecutedCount;
    time_t   blinkerOccurrenceStart;
};
Timer timers[MAX_TIMERS];

// weekMask bit 7 alone means every day; bits 0..6 mean Sunday..Saturday.
static bool timerRunsOnWeekday(const Timer &t, uint8_t weekday)
{
    if (t.weekMask & 0x80)
        return true;
    if (weekday > 6)
        return false;
    return (t.weekMask & (1U << weekday)) != 0;
}

//======================================================
//                  FIRMWARE VERSION
//======================================================

#define FW_VERSION_MAJOR     1
#define FW_VERSION_MINOR     0
#define FW_VERSION_BUILD     0

#define FW_BUILD_DATE        14050803
#define FW_BUILD_TIME        1000

//======================================================
//                    DEBUG
//======================================================

#define debug 1  // 1:true   0:false

#if debug

#define DBG_BEGIN(x)      Serial.begin(x)
#define DBG_PRINT(x)      Serial.print(x)
#define DBG_PRINTLN(x)    Serial.println(x)
#define DBG_PRINTF(...)   Serial.printf(__VA_ARGS__)

#else

#define DBG_BEGIN(x)
#define DBG_PRINT(x)
#define DBG_PRINTLN(x)
#define DBG_PRINTF(...)

#endif

//======================================================
//                  DEVICE CONSTANTS
//======================================================

#define MAX_WIFI                3
#define UDP_PORT                4210
const unsigned long CONNECT_TIMEOUT = 15000;
#define MAX_RETRY               3 // تعداد تلاش برای هر مودم

//======================================================
//                  MCP23017 REGISTERS
//======================================================

#define MCP_ADDR    0x20

#define IODIRA      0x00
#define IODIRB      0x01

#define GPPUA       0x0C
#define GPPUB       0x0D

#define GPIOA       0x12
#define GPIOB       0x13

#define OLATA       0x14
#define OLATB       0x15

//======================================================
//                  PORT SELECTION
//======================================================

#define TOUCH_PORT         GPIOA // پد های تاچ روی کدام پورت mcp متصل اند 
#define RELAY_PORT         GPIOB // رله ها روی کدام پورت mcp متصل اند
// #define TOUCH_COUNT        4 // تعداد رله و تاچهای هر دستگاه 
/*
اگر PCB عوض شود، فقط همین قسمت ابتدای فایل را تغییر می‌دهی:
const byte touchMap[] = {...};
const byte relayMap[] = {...};
#define TOUCH_PORT ...
#define RELAY_PORT ...
و دیگر هیچ جای دیگری از برنامه  نیاز به تغییر نیست .
*/
//======================================================
//                  GPIO MAP
//======================================================

#define BOOT_RELAY_MIN_DELAY_MS   1000UL // حداقل زمان توقف برای رله ها در زمان بوت شدن

bool relayTimeDependent[TOUCH_COUNT] = {false};
// false = منبعش تاچ/اپ/بک‌اند بوده (بدون نیاز به زمان)
// true  = منبعش Timer/Scheduler بوده (نیاز به زمان معتبر دارد)


const byte touchMap[TOUCH_COUNT] ={    0,    2,    1,    3}; // V3_PCB 14050505
const byte relayMap[TOUCH_COUNT] ={    0,    1,    2,    3}; // رله ها روی کدام پین های mcp متصل شده اند 
//=======================//2===============================
//                  WIFI STATE
//======================================================
enum WiFiManagerState_t
{
    WIFI_IDLE = 0,
    WIFI_CONNECTING,
    WIFI_CONNECTED
};

//======================================================
//                  WIFI STATUS LED MODE
//======================================================

#if MODULE_TYPE == MODULE_ESP12F

    #define LED_STA_PIN        12
    #define LED_AP_PIN         14
    #define SERVICE_KEY_PIN    13

    #define I2C_SDA            4
    #define I2C_SCL            5

#elif MODULE_TYPE == MODULE_ESP32C3

    #define LED_STA_PIN        99   // ⚠️ placeholder — با دیتاشیت برد واقعی جایگزین کنم
    #define LED_AP_PIN         99   // ⚠️ placeholder
    #define SERVICE_KEY_PIN    99   // ⚠️ placeholder

    #define I2C_SDA            99   // ⚠️ placeholder
    #define I2C_SCL            99   // ⚠️ placeholder

#endif
//======================================================
//                  STA LED MODE
//======================================================
enum StaLedMode_t
{
    STA_DISCONNECTED = 0,
    STA_CONNECTING,
    STA_CONNECTED
};
//======================================================
//                  AP MODE
//======================================================
enum APMode_t
{
    AP_OFF = 0,
    AP_ON
};
//======================================================
//                  BUTTON EVENT
//======================================================
enum ButtonEvent_t
{
    BUTTON_IDLE = 0,
    BUTTON_SHORT_PRESS,
    BUTTON_LONG_PRESS
};
//======================================================
//                  STRUCTURES
//======================================================
struct WiFiItem
{
    char ssid[32];
    char pass[64];
    bool enable;
};
struct Config
{
    char deviceName[16];

    char groupId[16];

    WiFiItem wifi[MAX_WIFI];

    byte wifiCount;

    byte lastConnectedIndex;   // آخرین اسلاتی که موفق به اتصال شدیم؛ اسلات 0 = کارخانه/نصاب
};
//======================================================
//                  OBJECTS
//======================================================
WebServerType server(80); 
WiFiUDP udp;
Config config;
//========================//3==============================
//                  GLOBAL VARIABLES
//======================================================

//--------------- Device ----------------

char hostName[32];
bool wifiConnected = false;
bool apEnabled = false;
//--------------- WiFi ------------------
byte currentWiFi = 0;
byte retryCount = 0;
unsigned long wifiTimer = 0;
WiFiManagerState_t WifiManagerState = WIFI_IDLE;
bool wifiBeginPending = false;
unsigned long wifiBeginPendingAt = 0;
//--------------- Provision -------------
bool wifiProvisionRunning = false;
bool wifiTrialActive = false;
WiFiItem trialWifi;
int trialTargetIndex = -1;
bool provisionFailed = false;
unsigned long wifiProvisionStart = 0;
String provisionSSID;
String provisionPASS;
//--------------- Touch -----------------
byte lastInputs = 0;
unsigned long lastInputScan = 0;
//--------------- Relay -----------------
bool relayState[TOUCH_COUNT] = {0};
// Flash snapshot is separate from live relay state; timer runtime changes are not persisted.
bool relaySavedState[TOUCH_COUNT] = {0};
bool relaySavedTimeDependent[TOUCH_COUNT] = {0};
bool relayNeedSave = false;
unsigned long relaySaveTimer = 0;

bool outputsReady = false;
unsigned long bootRelayDelayStart = 0;

//--------------- STA LED ---------------
StaLedMode_t staLedMode = STA_DISCONNECTED;
bool staLedState = false;
//--------------- AP LED ----------------
bool apLedState = false;
//--------------- Button ----------------
bool buttonPressed = false;
unsigned long buttonPressTime = 0;
ButtonEvent_t buttonEvent = BUTTON_IDLE;
//--------------- Status ----------------
unsigned long lastStatusPrint = 0;
unsigned long lastHeartBeat = 0;
//--------------- Debug -----------------
bool mdnsStarted = false;
//--------------- MCP -----------------
bool mcpOk = true;
byte lastMcpValue = 0;
//--------------- time -----------------
volatile bool   ntpSyncEvent     = false;
volatile time_t ntpSyncEventTime = 0;
bool schedulerCatchupApplied = false;


//======================//4================================
//                  PROTOTYPES
//======================================================
//---------------- Utility ----------------
byte makeInputMask(const byte *map, byte count);
byte makeOutputMask(const byte *map, byte count);
bool isWiFiConnected();
String getCurrentSSID();
IPAddress getDeviceIP();
int getRSSI();
void printFirmwareInfo();
//---------------- MCP23017 ----------------
bool mcpWrite(byte reg, byte value);
byte mcpRead(byte reg);
void initMCP();
void taskBootRelayRestore();
//---------------- Relay ----------------
void updateOutputs();
void saveRelayState();
void loadRelayState();
void handleRootV2();
void handleGetTimersV2();
void handleDevTime();
//---------------- Touch ----------------
void scanInputs();
//---------------- LEDs ----------------
void updateStatusLEDs();
//---------------- Button ----------------
void handleServiceButton();
//---------------- WiFi ----------------
void startWiFiConnection();
void startTrialConnection(String ssid, String pass, int targetIndex);
void nextWiFi();
void handleWiFiManager();
void initWiFi();
void processWiFiBegin();
void taskWiFiBegin();
void startProvisionConnection(int index);
void handleProvision();
void doFactoryReset();
void setAPMode(bool enable);
void handleEnableAP();
void handleDeleteWiFi();
//---------------- Web ---------------- 
void handleToggle();
void handleStatus();
void handleWiFiSetup();
void handleSaveWiFi();
void handleScanWiFi();
void handleFactoryReset(); 
void handleResult();
//---------------- UDP ----------------
void handleUDP();
void initUDP();
//---------------- Tasks ----------------
void taskWiFi();
void taskWebServer();
void taskUDP();
void taskLED();
void taskButton();
void taskRelaySave();
void taskStatus();
void runSystem();
void startSystem();
//------------14050803
void initCloudAPI();
void taskCloudSync();
bool isCloudServerReachableForTimeSync();
//---------------- Boot ----------------
void loadPersistentData();
void savePersistentData();
void initHealthManager();
void updateHealthManager();
void printHealthManager();
//---------------- Logs ----------------
void logSystem(const String &message);
//---------------- syne Time& Scheduler ----------------
void taskTimeManager();
void syncTime();
void initTimeManager();
void debugPrintTimeStatus();
void onTimeSynced(bool fromSntp);
void applyClientTimeSync(time_t clientUtc);
void checkClientTimeSync();
bool hasValidClock();
bool timeStateReady();

void taskScheduler();
void handleSetTimer();
void loadTimers();
void saveTimers();

//---------------- syne Time& Scheduler ----------------

void processBlinkerTimer(Timer &t, time_t now);

//========================//5==============================
//                  UTILITY FUNCTIONS
//======================================================
byte makeInputMask(const byte *map, byte count)
{
    byte mask = 0;

    for (byte i = 0; i < count; i++)
    {
        bitSet(mask, map[i]);
    }

    return mask;
}

//------------------------------------------------------

byte makeOutputMask(const byte *map, byte count)
{
    byte mask = 0;

    for (byte i = 0; i < count; i++)
    {
        bitSet(mask, map[i]);
    }

    return mask;
}

//------------------------------------------------------

bool isWiFiConnected()
{
    return (WiFi.status() == WL_CONNECTED);
}

//------------------------------------------------------

String getCurrentSSID()
{
    if (!isWiFiConnected())
        return "";

    return WiFi.SSID();
}

//------------------------------------------------------

IPAddress getDeviceIP()
{
    if (!isWiFiConnected())
        return IPAddress(0,0,0,0);

    return WiFi.localIP();
}

//------------------------------------------------------

int getRSSI()
{
    if (!isWiFiConnected())
        return 0;

    return WiFi.RSSI();
}

//------------------------------------------------------

void printFirmwareInfo()
{
    if (!debug)
        return;

    DBG_PRINTLN("");
    DBG_PRINTLN("======================================");
    DBG_PRINT("Firmware : ");
    DBG_PRINT(FW_VERSION_MAJOR);
    DBG_PRINT(".");
    DBG_PRINT(FW_VERSION_MINOR);
    DBG_PRINT(".");
    DBG_PRINTLN(FW_VERSION_BUILD);

    DBG_PRINT("Build : ");
    DBG_PRINT(FW_BUILD_DATE);
    DBG_PRINT(" ");
    DBG_PRINTLN(FW_BUILD_TIME);

    DBG_PRINTLN("======================================");
    DBG_PRINTLN("");
}

//========================//6==============================
//                  LITTLEFS
//======================================================

bool loadConfig()
{
    File f = LittleFS.open("/config.bin", "r");

    if (!f)
        return false;

    if (f.size() != sizeof(Config))
    {
        f.close();
        return false;
    }

    f.read((uint8_t *)&config, sizeof(Config));

    f.close();

    return true;
}

//------------------------------------------------------

bool saveConfig()
{
    File f = LittleFS.open("/config.bin", "w");

    if (!f)
        return false;

    f.write((uint8_t *)&config, sizeof(Config));

    f.close();

    return true;
}

//------------------------------------------------------

/*
void loadRelayState()
{
    File f = LittleFS.open("/relay.dat", "r");

    size_t expectedSize = sizeof(relayState) + sizeof(relayTimeDependent);

    if (!f)
    {
        memset(relayState, 0, sizeof(relayState));
        memset(relayTimeDependent, 0, sizeof(relayTimeDependent));
        return;
    }

    if (f.size() == expectedSize)
    {
        f.read((uint8_t *)relayState, sizeof(relayState));
        f.read((uint8_t *)relayTimeDependent, sizeof(relayTimeDependent));
    }
    else
    {
        memset(relayState, 0, sizeof(relayState));
        memset(relayTimeDependent, 0, sizeof(relayTimeDependent));
    }

    f.close();
}*/

void loadRelayState()
{
    File f = LittleFS.open("/relay.dat", "r");

    const size_t currentSize =
        sizeof(relaySavedState) + sizeof(relaySavedTimeDependent);
    const size_t oldestSize = sizeof(relaySavedState);

    if (!f)
    {
        memset(relaySavedState, 0, sizeof(relaySavedState));
        memset(relaySavedTimeDependent, 0, sizeof(relaySavedTimeDependent));
        memset(relayState, 0, sizeof(relayState));
        memset(relayTimeDependent, 0, sizeof(relayTimeDependent));
        return;
    }

    if (f.size() == currentSize || f.size() == currentSize + TOUCH_COUNT)
    {
        f.read((uint8_t *)relaySavedState, sizeof(relaySavedState));
        f.read((uint8_t *)relaySavedTimeDependent, sizeof(relaySavedTimeDependent));
    }
    else if (f.size() == oldestSize)
    {
        // Compatibility with the oldest format: relay states only.
        f.read((uint8_t *)relaySavedState, sizeof(relaySavedState));
        memset(relaySavedTimeDependent, 0, sizeof(relaySavedTimeDependent));
    }
    else
    {
        memset(relaySavedState, 0, sizeof(relaySavedState));
        memset(relaySavedTimeDependent, 0, sizeof(relaySavedTimeDependent));
    }

    f.close();

    // Runtime state starts from the last user-persisted snapshot.
    memcpy(relayState, relaySavedState, sizeof(relayState));
    memcpy(relayTimeDependent, relaySavedTimeDependent, sizeof(relayTimeDependent));
}
//------------------------------------------------------

/*
void saveRelayState()
{
    File f = LittleFS.open("/relay.dat", "w");

    if (!f)
        return;

    f.write((uint8_t *)relayState, sizeof(relayState));
    f.write((uint8_t *)relayTimeDependent, sizeof(relayTimeDependent));

    f.close();
}
*/
void saveRelayState()
{
    File f = LittleFS.open("/relay.dat", "w");

    if (!f)
        return;

    // Persist the snapshot, not live states changed by timers.
    f.write((uint8_t *)relaySavedState, sizeof(relaySavedState));
    f.write((uint8_t *)relaySavedTimeDependent, sizeof(relaySavedTimeDependent));
    f.close();
}


//========================//7==============================
//                  MCP23017
//======================================================

bool mcpWrite(byte reg, byte value)
{
    Wire.beginTransmission(MCP_ADDR);

    Wire.write(reg);
    Wire.write(value);

    byte err = Wire.endTransmission();

    bool ok = (err == 0);

    if (ok != mcpOk)
    {
        mcpOk = ok;

        if (debug)
        {
            if (mcpOk)
                DBG_PRINTLN("MCP23017 : OK (recovered)");
            else
            {
                DBG_PRINT("MCP23017 : I2C Write Error, code=");
                DBG_PRINTLN(err);
            }
        }
    }

    return ok;
}

//------------------------------------------------------

byte mcpRead(byte reg)
{
    Wire.beginTransmission(MCP_ADDR);
    Wire.write(reg);

    byte err = Wire.endTransmission(false);

    if (err != 0)
    {
        if (mcpOk)
        {
            mcpOk = false;

            if (debug)
            {
                DBG_PRINT("MCP23017 : I2C Error (write phase), code=");
                DBG_PRINTLN(err);
            }
        }

        return lastMcpValue;   // به‌جای صفر گمراه‌کننده، آخرین مقدار معتبر برگردونده می‌شه
    }

    byte got = Wire.requestFrom((uint8_t)MCP_ADDR, (uint8_t)1);

    if (got != 1 || !Wire.available())
    {
        if (mcpOk)
        {
            mcpOk = false;

            if (debug)
                DBG_PRINTLN("MCP23017 : I2C Error (no data received)");
        }

        return lastMcpValue;
    }

    byte value = Wire.read();

    if (!mcpOk)
    {
        mcpOk = true;

        if (debug)
            DBG_PRINTLN("MCP23017 : OK (recovered)");
    }

    lastMcpValue = value;

    return value;
}

//------------------------------------------------------

void mcpInit()
{
    byte touchMask = makeInputMask(touchMap, TOUCH_COUNT);

    byte relayMask = makeOutputMask(relayMap, TOUCH_COUNT);

    // GPA = Inputs

    mcpWrite(IODIRA, touchMask);

    // GPB = Outputs

    mcpWrite(IODIRB, (byte)~relayMask);

    // PullUp on Inputs

    mcpWrite(GPPUA, touchMask);

    // Outputs OFF

    mcpWrite(RELAY_PORT, 0);

    // Read Initial Inputs

    byte raw = mcpRead(TOUCH_PORT);

    lastInputs = 0;

    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        if (bitRead(raw, touchMap[i]))
        {
            bitSet(lastInputs, i);
        }
    }
}

//========================//8=============================
//                      RELAY
//======================================================

void updateOutputs()
{
    byte value = 0;

    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        // if (relayState[i])
        // {
        //     bitSet(value, relayMap[i]);
        // }
        if (relayState[i])
        {
            //==================================================
            // Relayهایی که وضعیتشان وابسته به زمان است
            // قبل از TIME_SYNCED نباید روی خروجی فیزیکی اعمال شوند.
            //
            // Manual relayها مستقل از زمان هستند و در بوت قابل Restore هستند.
            //==================================================
            if (relayTimeDependent[i] &&
                !hasValidClock())
            {
                continue;
            }

            bitSet(value, relayMap[i]);
        }
    }

    mcpWrite(RELAY_PORT, value);

    if (debug)
    {
        static byte lastValue = 0xFF;

        if (lastValue != value)
        {
            lastValue = value;

            DBG_PRINT("Relay Byte : 0b");

            for (int8_t b = 7; b >= 0; b--)
            {
                DBG_PRINT(bitRead(value, b));
            }

            DBG_PRINTLN("");

            String relayLog = "RELAY_STATE | BYTE=";
            relayLog += String(value);
            logSystem(relayLog);
        }
    }
}

//------------------------------------------------------
bool isOccurrenceCancelled(
    Timer &t,
    time_t occurrenceStart
)
{
    if (occurrenceStart == 0)
        return false;

    return t.cancelledOccurrenceSecond ==
           (uint32_t)occurrenceStart;
}

void cancelAutomationForRelay(byte relay)
{
    if (relay >= TOUCH_COUNT)
        return;

    bool changed = false;

    for (uint8_t i = 0; i < MAX_TIMERS; i++)
    {
        Timer &t = timers[i];

        if (!t.enabled)
            continue;

        if (t.relayIndex != relay)
            continue;

        // -------------------------
        // Normal Timer occurrence
        // -------------------------
if (t.mode == TIMER_MODE_NORMAL)
{
    time_t occurrenceStart = 0;

    // -------------------------
    // ONCE
    // -------------------------
    if (t.type == TIMER_ONCE)
    {
        occurrenceStart =
            timerDateTimeToEpoch(
                t.year,
                t.month,
                t.day,
                t.hour,
                t.minute,
                t.second
            );
    }

    // -------------------------
    // DAILY
    // -------------------------
    else if (t.type == TIMER_DAILY)
    {
        time_t now = getCurrentTime();

        struct tm *tmInfo =
            gmtime(&now);

        if (tmInfo)
        {
            time_t midnight =
                now -
                (tmInfo->tm_hour * 3600) -
                (tmInfo->tm_min * 60) -
                tmInfo->tm_sec;

            occurrenceStart =
                midnight +
                t.hour * 3600 +
                t.minute * 60 +
                t.second;

            // اگر occurrence امروز هنوز نرسیده،
            // occurrence جاری مربوط به دیروز است.
            if (occurrenceStart > now)
            {
                occurrenceStart -= 86400;
            }
        }
    }

    // -------------------------
    // WEEKLY
    // -------------------------
    else if (t.type == TIMER_WEEKLY)
    {
        time_t now = getCurrentTime();

        struct tm *tmInfo =
            gmtime(&now);

        if (tmInfo)
        {
            time_t midnight =
                now -
                (tmInfo->tm_hour * 3600) -
                (tmInfo->tm_min * 60) -
                tmInfo->tm_sec;

            occurrenceStart =
                midnight +
                t.hour * 3600 +
                t.minute * 60 +
                t.second;

            if (occurrenceStart > now)
            {
                occurrenceStart -= 86400;
            }

            // اگر روز محاسبه‌شده جزو
            // weekMask نیست، فعلاً occurrence
            // قابل لغوی نداریم.
            struct tm *occInfo =
                gmtime(&occurrenceStart);

            if (!occInfo ||
                !timerRunsOnWeekday(t, (uint8_t)occInfo->tm_wday))
            {
                occurrenceStart = 0;
            }
        }
    }

    // -------------------------
    // ثبت cancellation
    // -------------------------
    if (occurrenceStart != 0)
    {
        t.cancelledOccurrenceSecond =
            (uint32_t)occurrenceStart;

        t.activeUntil = 0;

        if (t.type == TIMER_ONCE)
            t.enabled = false;

        changed = true;
    }
}

        // -------------------------
        // Blinker occurrence
        // -------------------------
        if (t.mode == TIMER_MODE_BLINKER &&
            t.blinkStartTime != 0)
        {
            t.cancelledOccurrenceSecond =
                (uint32_t)t.blinkStartTime;

            t.blinkStartTime = 0;
            t.blinkEndTime = 0;
            t.blinkNextToggle = 0;
            t.blinkState = false;
            t.blinkerExecutedCount = 0;
            t.blinkerOccurrenceStart = 0;

            if (t.type == TIMER_ONCE)
                t.enabled = false;

            changed = true;
        }
    }

    if (changed)
        saveTimers();
}


//------------------------------------------------------
/*
void relayOn(byte index, bool timeDependent = false)
{
    if (index >= TOUCH_COUNT)
        return;

    relayTimeDependent[index] = timeDependent;

    if (!relayState[index])
    {
        relayState[index] = true;

        sys.relayNeedSave = true;

        updateOutputs();
    }
}

//------------------------------------------------------

void relayOff(byte index, bool timeDependent = false)
{
    if (index >= TOUCH_COUNT)
        return;

    relayTimeDependent[index] = timeDependent;

    if (relayState[index])
    {
        relayState[index] = false;

        sys.relayNeedSave = true;

        updateOutputs();
    }
}
*/
void markRelayStatePersistent(byte index)
{
    if (index >= TOUCH_COUNT)
        return;

    if (relayTimeDependent[index])
    {
        relaySavedState[index] = false;
        relaySavedTimeDependent[index] = true;
    }
    else
    {
        relaySavedState[index] = relayState[index];
        relaySavedTimeDependent[index] = false;
    }

    sys.relayNeedSave = true;
}

void relayOn(byte index, bool timeDependent = false)
{
    if (index >= TOUCH_COUNT)
        return;

    bool stateChanged = !relayState[index];
    bool modeChanged = relayTimeDependent[index] != timeDependent;

    relayTimeDependent[index] = timeDependent;

    if (stateChanged)
    {
        relayState[index] = true;
        updateOutputs();
    }

    if (timeDependent)
    {
        // Never restore an automation-produced ON state after reboot.
        if (!relaySavedTimeDependent[index] || relaySavedState[index])
        {
            relaySavedState[index] = false;
            relaySavedTimeDependent[index] = true;
            sys.relayNeedSave = true;
        }
    }
    else if (stateChanged || modeChanged)
    {
        markRelayStatePersistent(index);
    }

}

void relayOff(byte index, bool timeDependent = false)
{
    if (index >= TOUCH_COUNT)
        return;

  
  

    bool stateChanged = relayState[index];
    bool modeChanged = relayTimeDependent[index] != timeDependent;
   

    relayTimeDependent[index] = timeDependent;

    if (stateChanged)
    {
        relayState[index] = false;
        updateOutputs();
    }

    if (timeDependent)
    {
        // A timer OFF is runtime state, not a persisted user preference.
        if (!relaySavedTimeDependent[index] || relaySavedState[index])
        {
            relaySavedState[index] = false;
            relaySavedTimeDependent[index] = true;
            sys.relayNeedSave = true;
        }
    }
    else if (stateChanged || modeChanged)
    {
        markRelayStatePersistent(index);
    }

}
  

void relayToggle(byte index, bool timeDependent = false)
 
{
    if (index >= TOUCH_COUNT)
        return;

    relayState[index] = !relayState[index];
    relayTimeDependent[index] = timeDependent;
  

    updateOutputs();
 

    if (timeDependent)
    {
        if (!relaySavedTimeDependent[index] || relaySavedState[index])
        {
            relaySavedState[index] = false;
            relaySavedTimeDependent[index] = true;
            sys.relayNeedSave = true;
        }
    }
    else
    {
        markRelayStatePersistent(index);
    }

}

void manualRelayOn(byte index)
{
    if (index >= TOUCH_COUNT)
        return;

    cancelAutomationForRelay(index);
    relayOn(index, false);
    // Persist deliberate user intent even when the relay was already ON.
    markRelayStatePersistent(index);
  

  
}
  

void manualRelayOff(byte index)
{
    if (index >= TOUCH_COUNT)
        return;

    cancelAutomationForRelay(index);
    relayOff(index, false);
    markRelayStatePersistent(index);
}

  

void manualRelayToggle(byte index)
{
    if (index >= TOUCH_COUNT)
        return;

    cancelAutomationForRelay(index);
    relayToggle(index, false);
    markRelayStatePersistent(index);
}


  

bool relayGetState(byte index)
{
    if (index >= TOUCH_COUNT)
        return false;

    return relayState[index];
}

  

void allRelayOff(bool timeDependent = false)
{
    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        relayState[i] = false;
        relayTimeDependent[i] = timeDependent;

        if (timeDependent)
        {
            relaySavedState[i] = false;
            relaySavedTimeDependent[i] = true;
        }
        else
        {
            relaySavedState[i] = false;
            relaySavedTimeDependent[i] = false;
        }
    }

    sys.relayNeedSave = true;

    updateOutputs();
}

  

void allRelayOn(bool timeDependent = false)
{
    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        relayState[i] = true;
        relayTimeDependent[i] = timeDependent;

        if (timeDependent)
        {
            // Time-based ON must be reconstructed after clock sync.
            relaySavedState[i] = false;
            relaySavedTimeDependent[i] = true;
        }
        else
        {
            relaySavedState[i] = true;
            relaySavedTimeDependent[i] = false;
        }
    }

    sys.relayNeedSave = true;

    updateOutputs();
}


//=========================//9=============================
//                      TOUCH
//======================================================

void scanInputs()
{
    static unsigned long lastScan = 0;

    if (millis() - lastScan < 30)
        return;

    lastScan = millis();

    byte raw = mcpRead(TOUCH_PORT);

    byte inputs = 0;

    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        if (bitRead(raw, touchMap[i]))
            bitSet(inputs, i);
    }

    byte changed = lastInputs ^ inputs;

    if (changed == 0)
    {
        lastInputs = inputs;
        return;
    }

    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        if (!bitRead(changed, i))
            continue;

        bool oldState = bitRead(lastInputs, i);
        bool newState = bitRead(inputs, i);

        // PullUp : Released=1  Pressed=0

        if (oldState == 1 && newState == 0)
        {
            manualRelayToggle(i);

            if(debug)
            {
                DBG_PRINT("Touch ");
                DBG_PRINT(i + 1);

                if(relayGetState(i))
                    DBG_PRINTLN(" -> ON");
                else
                    DBG_PRINTLN(" -> OFF");
            }
        }
    }

    lastInputs = inputs;
}

//------------------------------------------------------

bool touchPressed(byte index)
{
    if(index >= TOUCH_COUNT)
        return false;

    return !bitRead(lastInputs, index);
}

//========================//10==============================
//                      LED
//======================================================

void setStaLed(bool state)
{
    staLedState = state;
    digitalWrite(LED_STA_PIN, state);
}

//------------------------------------------------------

void setApLed(bool state)
{
    apLedState = state;
    digitalWrite(LED_AP_PIN, state);
}

//------------------------------------------------------

void updateStatusLEDs()
{
    static unsigned long lastStaBlink = 0;
    static unsigned long lastApBlink = 0;
    static unsigned long lastHeartBeat = 0;

    //--------------------------------------------------
    // STA LED
    //--------------------------------------------------

    switch (staLedMode)
    {
        case STA_DISCONNECTED:
        {
            setStaLed(false);
            break;
        }

        case STA_CONNECTING:
        {
            if (millis() - lastStaBlink >= 200)
            {
                lastStaBlink = millis();

                setStaLed(!staLedState);
            }

            break;
        }

        case STA_CONNECTED:
        {
            if (millis() - lastHeartBeat >= 5000)
            {
                lastHeartBeat = millis();

                setStaLed(true);
            }

            if (staLedState &&
                millis() - lastHeartBeat >= 60)
            {
                setStaLed(false);
            }

            break;
        }
    } 
    //--------------------------------------------------
    // AP LED
    //-------------------------------------------------- 
    if (sys.apEnabled)
    {
        if (WiFi.softAPgetStationNum() > 0)
        {
            if (millis() - lastApBlink >= 500)
            {
                lastApBlink = millis();

                setApLed(!apLedState);
            }
        }
        else
        {
            setApLed(true);
        }
    }
    else
    {
        setApLed(false);
    }
}
    //--------------------------------------------------
    // AP mode from web server
    //--------------------------------------------------

void setAPMode(bool enable)
{
    sys.apEnabled = enable;

    if (sys.apEnabled)
    {
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("SmartSwitch_SW000001", "12345678");

        if (debug)
            DBG_PRINTLN("AP Enabled");
    }
    else
    {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);

        if (debug)
            DBG_PRINTLN("AP Disabled");
    }
}

//=========================//11=============================
//                      BUTTON
//======================================================

void handleServiceButton()
{
    static bool lastState = HIGH;

    bool state = digitalRead(SERVICE_KEY_PIN);

    //--------------------------------------------------
    // Press
    //--------------------------------------------------

    if (lastState == HIGH && state == LOW)
    {
        buttonPressTime = millis();

        buttonPressed = true;
    }

    //--------------------------------------------------
    // Release
    //--------------------------------------------------

    if (lastState == LOW && state == HIGH)
    {
        if (buttonPressed)
        {
            unsigned long pressTime = millis() - 

            buttonPressTime;

            buttonPressed = false;

            if (pressTime >= 5000)
            {
                buttonEvent = BUTTON_LONG_PRESS;

                if(debug)
                    DBG_PRINTLN("Button -> LONG");
            }
            else if (pressTime >= 80)
            {
                buttonEvent = BUTTON_SHORT_PRESS;

                if(debug)
                    DBG_PRINTLN("Button -> SHORT");
            }
        }
    }

    //--------------------------------------------------
    // Execute Event
    //--------------------------------------------------

    switch (buttonEvent)
    {
       case BUTTON_SHORT_PRESS: //  فشار برای ۸۰ میلی‌ثانیه تا ۵ ثانیه روشن و خاموش شدن ap
        {
            buttonEvent = BUTTON_IDLE;

            setAPMode(!sys.apEnabled);

            break;
        }
        case BUTTON_LONG_PRESS:// فشار طولانی مدت بیش از 5 ثانیه برای ریست فکتوری 
        {
            buttonEvent = BUTTON_IDLE;

            if (debug)
                DBG_PRINTLN("Factory Reset (button)");

            doFactoryReset();

            ESP.restart();

            break;
        }
        // کیس های بعدی برای حالتهای دیگر می تواند باشد مثلا فشار دادن طولانی مدت و ....

        default:
        break;
    }

    lastState = state;
}

//=========================//12=============================
//                      WIFI
//======================================================

void startWiFiConnection()
{
    if (config.wifiCount == 0)
    {
        sys.wifiConnected = false; 
        staLedMode = STA_DISCONNECTED; 
        WifiManagerState = WIFI_IDLE; 
        return;
    } 
          // if (persist.lastConnectedWiFi < config.wifiCount)
          //     sys.currentWiFi = persist.lastConnectedWiFi;
          // else
          //     sys.currentWiFi = 0; 

          byte searchCount = config.wifiCount; 

          while (searchCount--)
          {
              if (config.wifi[sys.currentWiFi].enable)
                  break; 
              sys.currentWiFi++; 
              if (sys.currentWiFi >= config.wifiCount)
                  sys.currentWiFi = 0;
          } 
          if (!config.wifi[sys.currentWiFi].enable)
          {
              WifiManagerState = WIFI_IDLE;
              staLedMode = STA_DISCONNECTED; 
              if(debug)
                  DBG_PRINTLN("No Enabled WiFi"); 
              return;
          }

  //  sys.retryCount = 0; 
    sys.wifiConnected = false; 
    WifiManagerState = WIFI_CONNECTING; 
    staLedMode = STA_CONNECTING; 
    WiFi.disconnect(); 
    wifiBeginPending = true;
    wifiBeginPendingAt = millis();   // 100ms دیگه WiFi.begin واقعی زده می‌شه، بدون توقف برنامه
} 
//------------------------------------------------------

void startTrialConnection(String ssid, String pass, int targetIndex)
{
    memset(&trialWifi, 0, sizeof(trialWifi));
    ssid.toCharArray(trialWifi.ssid, sizeof(trialWifi.ssid));
    pass.toCharArray(trialWifi.pass, sizeof(trialWifi.pass));

    trialTargetIndex = targetIndex;

    wifiTrialActive = true;
    provisionFailed = false;

    wifiProvisionRunning = true;
    wifiProvisionStart = millis();

    sys.wifiConnected = false;
    WifiManagerState = WIFI_CONNECTING;
    staLedMode = STA_CONNECTING;

    WiFi.disconnect();

    wifiBeginPending = true;
    wifiBeginPendingAt = millis();
}
//------------------------------------------------------
void processWiFiBegin()
{
    if (!wifiBeginPending)
        return;

    if (millis() - wifiBeginPendingAt < 100)
        return;

     wifiBeginPending = false;

    const char* ssid = wifiTrialActive ? trialWifi.ssid : config.wifi[sys.currentWiFi].ssid;
    const char* pass = wifiTrialActive ? trialWifi.pass : config.wifi[sys.currentWiFi].pass;

    WiFi.begin(ssid, pass);

    wifiTimer = millis();  // تایم‌اوت CONNECT_TIMEOUT از همین لحظه شروع می‌شه

    if (debug)
    {
        DBG_PRINTLN("");

        DBG_PRINT(wifiTrialActive ? "Connecting (Trial) : " : "Connecting : ");
        DBG_PRINTLN(ssid);

        if (!wifiTrialActive)
        {
            // DBG_PRINT("Current WiFi : ");
            // DBG_PRINTLN(sys.currentWiFi + 1);
            
              DBG_PRINT("Current WiFi Index : ");
              DBG_PRINTLN(sys.currentWiFi);

              DBG_PRINT("Persistent WiFi Index : ");
              DBG_PRINTLN(persist.lastConnectedWiFi);
        }
    }
}    
//------------------------------------------------------

void nextWiFi()
{
    sys.retryCount = 0;

    sys.currentWiFi++;

    if (sys.currentWiFi >= config.wifiCount)
        sys.currentWiFi = 0;

    startWiFiConnection();
}

//------------------------------------------------------

void handleWiFiManager()
{
    if (config.wifiCount == 0)
        return;

    switch (WifiManagerState)
    {
        //--------------------------------------------------
        case WIFI_IDLE:
        //--------------------------------------------------
            sys.retryCount = 0;
            startWiFiConnection();
            break;
        case WIFI_CONNECTING: 
        //--------------------------------------------------
            if (wifiBeginPending)
                break;
            if (WiFi.status() == WL_CONNECTED)
            {
                sys.wifiConnected = true;
                WifiManagerState = WIFI_CONNECTED;
                staLedMode = STA_CONNECTED;
                sys.retryCount = 0;
                if (wifiTrialActive)
                {
                    // موفقیت‌آمیز؛ فقط الان روی فلش ذخیره می‌کنیم
                    wifiTrialActive = false;
                    sys.provisioningRunning = false; 
                    memset(&config.wifi[trialTargetIndex], 0, sizeof(WiFiItem));
                    strcpy(config.wifi[trialTargetIndex].ssid, trialWifi.ssid);
                    strcpy(config.wifi[trialTargetIndex].pass, trialWifi.pass);
                    config.wifi[trialTargetIndex].enable = true;

                    if (trialTargetIndex >= config.wifiCount)
                        config.wifiCount = trialTargetIndex + 1; 
                    
                    sys.currentWiFi = trialTargetIndex; 
                    persist.lastConnectedWiFi = sys.currentWiFi;
                      
                      saveConfig();
                      savePersistentData(); 
                      DBG_PRINT("Saved Last WiFi Index : ");
                      DBG_PRINTLN(persist.lastConnectedWiFi);

                    if (debug)
                        DBG_PRINTLN("Trial Success - Saved");
                }
                else if (persist.lastConnectedWiFi != sys.currentWiFi)
                {
                    // config.lastConnectedIndex = sys.currentWiFi;
                    // saveConfig();
                     persist.lastConnectedWiFi = sys.currentWiFi;
                     savePersistentData();
                     DBG_PRINT("Saved Last WiFi Index : ");
                     DBG_PRINTLN(persist.lastConnectedWiFi);
                }
                if (!sys.mdnsStarted)
                {
                    if (MDNS.begin(hostName))
                    {
                        MDNS.addService("http", "tcp", 80);
                        sys.mdnsStarted = true;
                    }
                }
                if (debug)
                {
                    DBG_PRINTLN("");
                    DBG_PRINTLN("WiFi Connected");
                    DBG_PRINT("SSID : ");
                    DBG_PRINTLN(WiFi.SSID());
                    DBG_PRINT("IP : ");
                    DBG_PRINTLN(WiFi.localIP());
                    DBG_PRINT("RSSI : ");
                    DBG_PRINTLN(WiFi.RSSI());
                }
                  String wifiLog = "WIFI_CONNECTED | SSID=";
                  wifiLog += WiFi.SSID();
                  wifiLog += " | IP=";
                  wifiLog += WiFi.localIP().toString();
                  logSystem(wifiLog);

                break;
            }
            if (millis() - wifiTimer >= CONNECT_TIMEOUT)
            {
                if (debug)
                {
                    DBG_PRINT("Connect Failed, status code = ");
                    DBG_PRINTLN(WiFi.status());
                }
                sys.retryCount++;
                if (wifiTrialActive)
                {
                    if (sys.retryCount >= MAX_RETRY)
                    {
                        // تست شکست خورد؛ هیچی ذخیره نمی‌شه، برمی‌گردیم به شبکه‌ی قبلی
                        wifiTrialActive = false;
                        provisionFailed = true;
                        sys.provisioningRunning = false;
                        sys.retryCount = 0; 
                        if (debug)
                            DBG_PRINTLN("Trial Failed - Reverting to previous network");
                        startWiFiConnection();
                    }
                    else
                    {
                        startTrialConnection(String(trialWifi.ssid), String(trialWifi.pass), trialTargetIndex);
                    }
                }
                else
                {
                    if (sys.retryCount >= MAX_RETRY)
                        nextWiFi();
                    else
                        startWiFiConnection();
                }
            }

            break;
        case WIFI_CONNECTED:
        //--------------------------------------------------

            if (WiFi.status() != WL_CONNECTED)
            {
                sys.wifiConnected = false;

                sys.mdnsStarted = false;

                staLedMode = STA_CONNECTING;

                WifiManagerState = WIFI_CONNECTING;

                sys.retryCount = 0;

                startWiFiConnection();
            }

            break;
    }
}


//=========================//13=============================
//                    PROVISION
//======================================================

void startProvisionConnection(int index)
{
    sys.currentWiFi = index;

    sys.retryCount = 0;

    sys.provisioningRunning = true;
    wifiProvisionStart = millis();

    startWiFiConnection();
}

//------------------------------------------------------

void handleProvision()
{
    if (!sys.provisioningRunning)
        return;

    if (WiFi.status() == WL_CONNECTED)
    {
        sys.provisioningRunning = false;

        if (debug)
            DBG_PRINTLN("Provision Success");

        return;
    }

    if (millis() - wifiProvisionStart > CONNECT_TIMEOUT * MAX_RETRY)
    {
        sys.provisioningRunning = false;

        if (debug)
            DBG_PRINTLN("Provision Timeout (WiFi Manager keeps retrying in background)");
    }
}

//==========================//14============================
//                        UDP
//======================================================

void handleUDP()
{
    int packetSize = udp.parsePacket();

    if (packetSize <= 0)
        return;

    char packet[256];

    int len = udp.read(packet, sizeof(packet) - 1);

    if (len <= 0)
        return;

    packet[len] = 0;

    if (debug)
    {
        DBG_PRINT("UDP RX : ");
        DBG_PRINTLN(packet);
    }

    //--------------------------------------------------
    // Discovery
    //--------------------------------------------------

    if (strcmp(packet, "DISCOVER") == 0)
    {
        String reply;

        reply.reserve(128);

        reply += "{";

        reply += "\"device\":\"";
        reply += config.deviceName;
        reply += "\",";

        reply += "\"group\":\"";
        reply += config.groupId;
        reply += "\",";

        reply += "\"ip\":\"";
        reply += WiFi.localIP().toString();
        reply += "\",";

        reply += "\"mac\":\"";
        reply += WiFi.macAddress();
        reply += "\",";

        reply += "\"i2c\":\"";
        reply += mcpOk ? "OK" : "FAIL";
        reply += "\"";

        reply += "}";

        udp.beginPacket(
            udp.remoteIP(),
            udp.remotePort());

        udp.print(reply);

        udp.endPacket();

        if (debug)
        {
            DBG_PRINTLN("UDP TX -> DISCOVER");
        }

        return;
    }

    //--------------------------------------------------
    // Ping
    //--------------------------------------------------

    if (strcmp(packet, "PING") == 0)
    {
        udp.beginPacket(
            udp.remoteIP(),
            udp.remotePort());

        udp.print("PONG");

        udp.endPacket();

        return;
    }
  
    //--------------------------------------------------
    // time sync  (اپلیکیشن یا هر ابزار UDP باید پکتی مثل TIME:1786345000 بفرسته)

    //--------------------------------------------------  
  if (strncmp(packet, "TIME:", 5) == 0)
  {
      time_t clientUtc = (time_t)atol(packet + 5);

      applyClientTimeSync(clientUtc);

      udp.beginPacket(udp.remoteIP(), udp.remotePort());
      udp.print("TIME_OK");
      udp.endPacket();

      return;
  }

}

//=========================//15=============================
//                    WEB SERVER
//======================================================

void handleStatus()
{
    // Preserve Client/App time sync when UTC is sent with /status.
    checkClientTimeSync();

    String json = "{";

    json += "\"ssid\":\"";
    json += WiFi.SSID();
    json += "\",";

    json += "\"ip\":\"";
    json += WiFi.localIP().toString();
    json += "\",";

    json += "\"rssi\":";
    json += String(WiFi.RSSI());
    json += ",";

    json += "\"relay\":[";

    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        json += relayState[i] ? "1" : "0";

        if (i < TOUCH_COUNT - 1)
            json += ",";
    }

    json += "]";

    json += "}";

    server.send(200, "application/json", json);
}

//------------------------------------------------------

void handleToggle()
{
    if (!server.hasArg("ch"))
    {
        server.send(400, "text/plain", "Bad Request");
        return;
    }

    byte id = server.arg("ch").toInt();

    if (id >= TOUCH_COUNT)
    {
        server.send(400, "text/plain", "Bad Index");
        return;
    }

    // relayToggle(id);
    manualRelayToggle(id);

    server.send(200, "text/plain", "OK");
}

//------------------------------------------------------

void handleResult()
{
    String page;

    page.reserve(512);

    page += "<html><head>";
    page += "<meta http-equiv='refresh' content='2'>";
    page += "</head><body>";

    page += "<h2>WiFi Status</h2><hr>";

    page += "<b>SSID : </b>";
    page += WiFi.SSID();

    page += "<br><b>IP : </b>";
    page += WiFi.localIP().toString();

    page += "<br><b>RSSI : </b>";
    page += String(WiFi.RSSI());

    page += "<br><br>";

  if (provisionFailed)
    {
        page += "<font color='red'><b>FAILED — Wrong password or network unreachable. Please try again.</b></font>";

        provisionFailed = false;   // پیام فقط یه‌بار نشون داده بشه
    }
    else if (WiFi.status() == WL_CONNECTED)
    {
        page += "<font color='green'><b>CONNECTED</b></font>";
    }
    else
    {
        page += "<font color='red'><b>CONNECTING...</b></font>";
    }

    page += "<br><br>";

    page += "<a href='/'>Back</a>";

    page += "</body></html>";

    server.send(200, "text/html", page);
}
//------------------------------------------------------
void doFactoryReset()
{
    WiFiItem factoryWifi = config.wifi[0];   // اسلات کارخانه/نصاب رو قبل از پاک کردن نگه می‌داریم

    WiFi.disconnect(true);

    LittleFS.remove("/config.bin");
    LittleFS.remove("/relay.dat");

    memset(&config, 0, sizeof(config));

    config.wifi[0] = factoryWifi;
    config.wifiCount = 1;
    config.lastConnectedIndex = 0;

    strcpy(config.deviceName, "SW000001");
    strcpy(config.groupId, "HOME");

    saveConfig();
}

//------------------------------------------------------

void handleFactoryReset()
{
    server.send(200, "text/html",
        "<html><body><h3>Factory Reset... Rebooting</h3></body></html>");

    delay(300);

    doFactoryReset();

    ESP.restart();
}

//------------------------------------------------------
void handleWiFiSetup()
{
    String html;

    html += R"rawliteral(
        <!DOCTYPE html>
        <html>
        <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <title>WiFi Setup</title>
        </head>
        <body>
        <h2>WiFi Setup</h2>
        <button onclick="scan()">Scan WiFi</button>
        <button onclick="enableAP()">Enable AP</button>
        <br><br>
        <form action="/savewifi" method="POST">
        SSID<br>
        <select id="ssid" style="width:240px;"></select>
        <input type="hidden" id="ssidValue" name="ssid">
        <br><br>
        Password<br>
        <input type="password" id="pass" name="pass" style="width:240px;">
        <br><br>
        <input type="submit" value="Connect"
              onclick="document.getElementById('ssidValue').value=document.getElementById('ssid').value;">
        </form>
        <hr>
        <h3>Stored WiFi</h3>
        )rawliteral";

            for (int i = 0; i < config.wifiCount; i++)
            {
                html += String(i);
                html += " : ";
                html += config.wifi[i].ssid;

                if (i == 0)
                {
                    html += " (Factory)";
                }
                else
                {
                    html += " <a href='/deletewifi?ch=";
                    html += String(i);
                    html += "' onclick=\"return confirm('Delete this network?');\">[Delete]</a>";
                }

                html += "<br>";
            }

            html += "<hr>";
            html += "<b>Current SSID :</b> ";
            html += getCurrentSSID();
            html += "<br><b>Current IP :</b> ";
            html += getDeviceIP().toString();
            html += "<br><b>RSSI :</b> ";
            html += String(getRSSI());
            html += "<hr>";
            html += "<b>Device Name :</b> ";
            html += config.deviceName;
            html += "<br><br>";
            html += "<a href='/factory' onclick=\"return confirm('Factory Reset ?');\">Factory Reset</a>"; 
            html += R"rawliteral( 
            html +=fetch('/timesync?utc=' + Math.floor(Date.now()/1000));
            scan();

        <script>
        function scan()
        {
            fetch('/scan')
            .then(r => r.json())
            .then(data => {
                let s = document.getElementById('ssid');
                s.innerHTML = "";
                data.forEach(function(w){
                    let o = document.createElement("option");
                    o.value = w.ssid;
                    o.text = w.ssid + " (" + w.rssi + " dBm)";
                    s.appendChild(o);
                });
            });
        }

        function enableAP()
        {
            fetch('/enableap')
            .then(r => r.text())
            .then(msg => alert(msg));
        }

        scan();
        </script>
        </body>
        </html>
        )rawliteral";

            server.send(200, "text/html", html);
}

//------------------------------------------------------

void handleScanWiFi()
{
    int n = WiFi.scanNetworks();

    String json = "[";

    for (int i = 0; i < n; i++)
    {
        if (i) json += ",";
        json += "{\"ssid\":\"";
        json += WiFi.SSID(i);
        json += "\",\"rssi\":";
        json += String(WiFi.RSSI(i));
        json += "}";
    }

    json += "]";

    server.send(200, "application/json", json);
}
//------------------------------------------------------
void handleEnableAP()
{
    setAPMode(true);

    server.send(200, "text/plain", "AP Enabled");
}
//------------------------------------------------------
void handleSaveWiFi()
{
    if (!server.hasArg("ssid") || !server.hasArg("pass"))
    {
        server.send(400, "text/plain", "Bad Request");
        return;
    }

    String ssid = server.arg("ssid");
    String pass = server.arg("pass");

    int targetIndex = -1;

    for (int i = 0; i < config.wifiCount; i++)
    {
        if (ssid.equals(config.wifi[i].ssid))
        {
            targetIndex = i;   // این SSID از قبل ذخیره شده؛ همین اسلات رو با پسورد جدید تست/آپدیت می‌کنیم
            break;
        }
    }

    if (targetIndex < 0)
    {
        // SSID کاملاً جدیده؛ یه اسلات تازه (یا آخرین اسلات در صورت پر بودن لیست) در نظر می‌گیریم
        targetIndex = (config.wifiCount < MAX_WIFI) ? config.wifiCount : (MAX_WIFI - 1);
    }
      
    sys.retryCount = 0;
    startTrialConnection(ssid, pass, targetIndex);

    server.sendHeader("Location", "/result");
    server.send(302, "text/plain", "");
}
//------------------------------------------------------
void handleDeleteWiFi()
{
    if (!server.hasArg("ch"))
    {
        server.send(400, "text/plain", "Bad Request");
        return;
    }

    int idx = server.arg("ch").toInt();

    if (idx <= 0 || idx >= config.wifiCount)   // اسلات 0 (کارخانه) هیچ‌وقت قابل حذف نیست
    {
        server.send(400, "text/plain", "Cannot Delete This Slot");
        return;
    }

    for (int i = idx; i < config.wifiCount - 1; i++)
        config.wifi[i] = config.wifi[i + 1];

    memset(&config.wifi[config.wifiCount - 1], 0, sizeof(WiFiItem));

    config.wifiCount--;

    if (sys.currentWiFi == idx)
        sys.currentWiFi = 0;         // شبکه‌ی فعلی حذف شد؛ برگرد به اسلات کارخانه (امن‌ترین گزینه)
    else if (sys.currentWiFi > idx)
        sys.currentWiFi--;           // چون لیست شیفت شد، اندیس‌ها یکی عقب میان

    if (config.lastConnectedIndex == idx)
        config.lastConnectedIndex = 0;
    else if (config.lastConnectedIndex > idx)
        config.lastConnectedIndex--;

    saveConfig();

    server.sendHeader("Location", "/wifi");
    server.send(302, "text/plain", "");
}
//------------------------------------------------------
/*
void handleSetTimer()
{
    if (!server.hasArg("slot"))
    {
        server.send(400, "text/plain", "Missing slot");
        return;
    }

    uint8_t slot = server.arg("slot").toInt();

    if (slot >= MAX_TIMERS)
    {
        server.send(400, "text/plain", "Invalid slot");
        return;
    }

    Timer t;
    memset(&t, 0, sizeof(t));
    t.enabled         = server.hasArg("enabled") ? (server.arg("enabled").toInt() != 0) : false;
    t.type             = (TimerType)server.arg("type").toInt();
    t.hour             = server.arg("hour").toInt();
    t.minute           = server.arg("minute").toInt();
    t.weekMask         = server.hasArg("weekMask") ? server.arg("weekMask").toInt() : 0;
    t.relayIndex       = server.arg("relay").toInt();
    t.relayState       = (server.arg("state").toInt() != 0);
    t.durationSeconds  = server.hasArg("duration") ? server.arg("duration").toInt() : 0;

    if (t.type > TIMER_WEEKLY ||
        t.hour > 23 || t.minute > 59 ||
        t.relayIndex >= TOUCH_COUNT)
    {
        server.send(400, "text/plain", "Invalid parameters");
        return;
    }

    timers[slot] = t;

    saveTimers();

    server.send(200, "text/plain", "OK");
}
*/
void handleSetTimer()
{
    if (!server.hasArg("slot"))
    {
        server.send(400, "text/plain", "Missing slot");
        return;
    }

    uint8_t slot = server.arg("slot").toInt();

    if (slot >= MAX_TIMERS)
    {
        server.send(400, "text/plain", "Invalid slot");
        return;
    }

    Timer t;
    memset(&t, 0, sizeof(t));

    //==================================================
    // BASIC TIMER PARAMETERS
    //==================================================

    t.enabled =
        server.hasArg("enabled") ?
        (server.arg("enabled").toInt() != 0) :
        false;

    t.type =
        (TimerType)server.arg("type").toInt();


          t.year =
              server.arg("year").toInt();

          t.month =
              server.arg("month").toInt();

          t.day =
              server.arg("day").toInt();

          t.hour =
              server.arg("hour").toInt();

          t.minute =
              server.arg("minute").toInt();

          t.second =
              server.arg("second").toInt();


    t.weekMask =
        server.hasArg("weekMask") ?
        server.arg("weekMask").toInt() :
        0;

    t.relayIndex =
        server.arg("relay").toInt();

    t.relayState =
        (server.arg("state").toInt() != 0);

  // 14050706
    t.durationSeconds =
        server.hasArg("duration")
            ? server.arg("duration").toInt()
            : 0;
        // 14050706
    //==================================================
    // TIMER MODE
    //==================================================

    t.mode =
        server.hasArg("mode") ?
        server.arg("mode").toInt() :
        TIMER_MODE_NORMAL;


    //==================================================
    // BLINKER PARAMETERS
    //==================================================

    if (t.mode == TIMER_MODE_BLINKER)
    {
        if (!server.hasArg("blinkOn") ||
            !server.hasArg("blinkOff") ||
            !server.hasArg("blinkerCount"))
        {
            server.send(
                400,
                "text/plain",
                "Missing Blinker parameters"
            );

            return;
        }

        t.blinkOnSeconds =
            server.arg("blinkOn").toInt();

        t.blinkOffSeconds =
            server.arg("blinkOff").toInt();

        t.blinkerCount =
            server.arg("blinkerCount").toInt();


        //==============================================
        // BLINKER VALIDATION
        //==============================================

        if (t.blinkOnSeconds < BLINK_MIN_SECONDS ||
            t.blinkOnSeconds > BLINK_MAX_SECONDS)
        {
            server.send(
                400,
                "text/plain",
                "Invalid Blinker ON duration"
            );

            return;
        }

        if (t.blinkOffSeconds < BLINK_MIN_SECONDS ||
            t.blinkOffSeconds > BLINK_MAX_SECONDS)
        {
            server.send(
                400,
                "text/plain",
                "Invalid Blinker OFF duration"
            );

            return;
        }

        if (t.blinkerCount == 0 || t.blinkerCount > 255)
        {
            server.send(
                400,
                "text/plain",
                "Blinker count must be 1..255 (255 = repeat forever)"
            );

            return;
        }
    }


    //==================================================
    // GENERAL VALIDATION
    //==================================================

    if (t.type > TIMER_WEEKLY ||
        t.year > 99 ||
        t.month < 1 || t.month > 12 ||
        t.day < 1 || t.day > 31 ||
        t.hour > 23 ||
        t.minute > 59 ||
        t.second > 59 ||
        t.relayIndex >= TOUCH_COUNT ||
        (slot / 2) != t.relayIndex ||
        (t.mode != TIMER_MODE_NORMAL &&
        t.mode != TIMER_MODE_BLINKER))
    {
        server.send(
            400,
            "text/plain",
            "Invalid parameters"
        );

        return;
    }


    //==================================================
    // TIMER ID: high nibble = relay index, low nibble = timer index (0..1)
    //==================================================
    t.timerId = (uint8_t)(((t.relayIndex & 0x0F) << 4) | (slot % 2));

    //==================================================
    // SAVE TIMER
    //==================================================

    timers[slot] = t;

    saveTimers();

    server.send(
        200,
        "text/plain",
        "OK"
    );
}

//=========================//16=============================
//                      TASKS
//======================================================

void taskWiFi()
{
    handleWiFiManager();
}

//------------------------------------------------------

void taskProvision()
{
    handleProvision();
}

//------------------------------------------------------

void taskWebServer()
{
    server.handleClient();
}

//------------------------------------------------------

void taskUDP()
{
    if (sys.wifiConnected)
    {
        if (sys.mdnsStarted)
            MDNS.update();

        handleUDP();
    }
}

//------------------------------------------------------

void taskTouch()
{
    scanInputs();
}

//------------------------------------------------------

void taskLED()
{
    updateStatusLEDs();
}

//------------------------------------------------------

void taskButton()
{
    handleServiceButton();
}

//------------------------------------------------------

void taskRelaySave()
{
    if (!sys.relayNeedSave)
        return;

    if (millis() - relaySaveTimer < 1000)
        return;

    relaySaveTimer = millis();

    sys.relayNeedSave = false; // برای ذخیره کردن وضعیت رله ها ست

    saveRelayState();

    if(debug)
        DBG_PRINTLN("Relay State Saved");
}

//------------------------------------------------------

void taskStatus()
{
    if (!debug)
        return;

    if (millis() - lastStatusPrint < 5000)
        return;

    lastStatusPrint = millis();

    DBG_PRINT("WiFi : ");

    if (sys.wifiConnected)
    {
        DBG_PRINT(WiFi.SSID());

        DBG_PRINT("  IP :");

        DBG_PRINT(WiFi.localIP());

        DBG_PRINT("  RSSI : ");

        DBG_PRINTLN(WiFi.RSSI());
    }
    else
    {
        DBG_PRINTLN("Disconnected");
    }
}

//======================//17================================
//                  Setup
//======================================================


void setup()
{
    DBG_BEGIN(115200);

    delay(500);

    printFirmwareInfo();

    startSystem();
  // debugReadSystemLog();
    DBG_PRINTLN("System Ready");
    // logSystem("SYSTEM_READY_for_LOGs");



}

//======================//18================================
//                  LOOP
//======================================================

void loop()
{
    runSystem();
}

//======================//19================================
//                  init
//======================================================

void initGPIO()
{
    pinMode(LED_STA_PIN, OUTPUT);
    pinMode(LED_AP_PIN, OUTPUT);

    pinMode(SERVICE_KEY_PIN, INPUT_PULLUP);

    setStaLed(false);
    setApLed(false);
}

//-------------------------BootRelayRestore-----pwm------------------------

void taskBootRelayRestore()
{
    if (outputsReady)
        return;

    if (millis() - bootRelayDelayStart < BOOT_RELAY_MIN_DELAY_MS)
        return;

    outputsReady = true;

    updateOutputs();   // فقط رله‌های تاچ/اپ را با مقدار واقعی‌شان می‌فرستد؛ زمان‌محورها همچنان false هستند

    if (debug)
        DBG_PRINTLN("Boot Relay Restore: outputs enabled");
}
//------------------------------------------------------

void initStorage()
{
    LittleFS.begin();

    if (!loadConfig())
    {
        strcpy(config.deviceName, "SW000001");
        strcpy(config.groupId, "HOME");

        memset(&config.wifi[0], 0, sizeof(WiFiItem));
        strcpy(config.wifi[0].ssid, "kftt");//"iot_installer");
        strcpy(config.wifi[0].pass, "44717764");//"12345678"); //
        config.wifi[0].enable = true;

        config.wifiCount = 1;
        config.lastConnectedIndex = 0;

        saveConfig();
    }


    snprintf(hostName, sizeof(hostName), "%s", config.deviceName);   // برای hostname بصورت پویا لحاظ شده

    sys.currentWiFi = config.lastConnectedIndex; // ?????

    if (sys.currentWiFi >= config.wifiCount)
        sys.currentWiFi = 0;

    loadRelayState();

    // Safety policy: all relay outputs start OFF after every reboot.
    // After the clock becomes valid, applyCatchupState() may restore only
    // a still-valid, non-cancelled timer occurrence.
    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        relayState[i] = false;
        relayTimeDependent[i] = false;
        relaySavedState[i] = false;
        relaySavedTimeDependent[i] = false;
    }
    sys.relayNeedSave = true;

    loadTimers(); // for timer in boot catchup

}

//------------------------------------------------------

void initMCP()
{
    Wire.begin(I2C_SDA, I2C_SCL);
    mcpInit();

    bootRelayDelayStart = millis();
}

//------------------------------------------------------

void initWiFi()
{
  
    WiFi.persistent(false);   // ← این خط جدیده؛ جلوی نوشتن غیرضروری روی فلش داخلی SDK رو می‌گیره

    WiFi.mode(WIFI_STA);

    sys.apEnabled = false;

      #if MODULE_TYPE == MODULE_ESP12F
          WiFi.setSleepMode(WIFI_NONE_SLEEP);
      #elif MODULE_TYPE == MODULE_ESP32C3
          WiFi.setSleep(false);
      #endif
 
    if (persist.lastConnectedWiFi < config.wifiCount)
        sys.currentWiFi = persist.lastConnectedWiFi;
    else
        sys.currentWiFi = 0;
     
    WifiManagerState = WIFI_IDLE;
}

//------------------------------------------------------

void initUDP()
{
    udp.begin(UDP_PORT);
}

//------------------------------------------------------

void initWebServer()
{
    // server.on("/", handleRoot);
    server.on("/toggle", handleToggle);
    server.on("/status", handleStatus);
// 14050706
	server.on("/", handleRootV2);
	server.on("/getTimers", handleGetTimersV2);
	server.on("/devtime", handleDevTime);
// 14050706
    server.on("/wifi", handleWiFiSetup);
    server.on("/savewifi", HTTP_POST, handleSaveWiFi);
    server.on("/scan", handleScanWiFi);
    server.on("/enableap", handleEnableAP);
    server.on("/result", handleResult);
    server.on("/factory", handleFactoryReset);
    server.on("/deletewifi", handleDeleteWiFi);
    server.on("/timesync", handleTimeSync);
    server.on("/setTimer", handleSetTimer);



    server.begin();
}

//========================//20==============================
//                  SYSTEM START
//======================================================


void startSystem()
{
    memset(&sys, 0, sizeof(sys));

    initGPIO();

    initStorage();

    initCloudAPI();

    loadPersistentData();

    initBootManager();


    initMCP();
    initWiFi();
    initTimeManager();

    initUDP();
    initWebServer();
}

//------------------------------------------------------
void taskWiFiBegin()
{
    processWiFiBegin();
}

//------------------------------------------------------

void runSystem()
{
  
    taskWiFiBegin(); 
    taskWiFi();
    updateHealthManager();
    printHealthManager(); 
    taskProvision(); 
        taskBootRelayRestore();      // تأخیر ایمن پیش از فعال‌سازی خروجی رله‌ها
                    taskTimeManager();

          //       taskTimeManager();
          //           taskScheduler();      // <--  مربوط بهScheduler این خط اضافه شد


          // taskWebServer(); 
          // taskUDP(); 
          // taskButton(); 
          // taskTouch(); 
          //==================================================
    // AUTOMATION GATE
    // Timer / Blinker
    // فقط بعد از معتبر شدن زمان اجرا شوند
    //==================================================

    if (hasValidClock())
  
        taskScheduler();

    
// ارتباط با سرور
taskCloudSync();
  
    // -----------------------------
    // MANUAL INPUTS — HIGHEST PRIORITY
    // -----------------------------
                    taskWebServer();
                    taskUDP(); 
                    taskButton();
                    taskTouch();    
                    taskLED(); 
                    taskRelaySave();

    taskStatus(); 
    yield();
}


//======================================================
//                  BOOT MANAGER
//======================================================

void initBootManager()
{
    memset(&boot, 0, sizeof(boot));

    boot.lastBootMillis = millis();

    boot.lastResetReason = ESP.getResetInfoPtr()->reason;

    persist.bootCounter++;

    persist.lastResetReason = boot.lastResetReason;
    boot.bootCounter = persist.bootCounter;
    boot.restartCounter = persist.restartCounter;
    boot.crashCounter = persist.crashCounter;


    savePersistentData();
    
      String bootLog = "BOOT | Reset=";
      bootLog += getResetReasonText(boot.lastResetReason);
      bootLog += " | Counter=";
      bootLog += String(persist.bootCounter);
      logSystem(bootLog);

  if(debug)
  {
      if(LittleFS.exists(PERSIST_FILE))
          DBG_PRINTLN("runtime.dat Created");
      else
          DBG_PRINTLN("runtime.dat NOT Created");
  }

  File f = LittleFS.open(PERSIST_FILE, "r");

  if(f)
  {
      DBG_PRINT("Runtime Size : ");
      DBG_PRINTLN(f.size());
      f.close();
  }


    if(debug)
    {
        DBG_PRINT("Reset Reason : ");
        DBG_PRINTLN(getResetReasonText(boot.lastResetReason));

        DBG_PRINT("Boot Counter : ");
        DBG_PRINTLN(persist.bootCounter);
      }
}

String getResetReasonText(uint8_t reason)
{
    switch(reason)
    {
        case REASON_DEFAULT_RST:      return "PowerOn";
        case REASON_WDT_RST:          return "HardwareWDT";
        case REASON_EXCEPTION_RST:    return "Exception";
        case REASON_SOFT_WDT_RST:     return "SoftwareWDT";
        case REASON_SOFT_RESTART:     return "Restart";
        case REASON_DEEP_SLEEP_AWAKE: return "DeepSleep";
        case REASON_EXT_SYS_RST:      return "ExternalReset";
    }

    return "Unknown";
}

void loadPersistentData()
{
    memset(&persist,0,sizeof(persist));

    if(!LittleFS.exists(PERSIST_FILE))
    {
        persist.dataVersion = PERSIST_VERSION;
        return;
    }

    File f = LittleFS.open(PERSIST_FILE,"r");

    if(!f)
        return;

    if(f.size()==sizeof(PersistentData))
    {
        f.read((uint8_t*)&persist,sizeof(PersistentData));
    }

    f.close();

    if(persist.dataVersion!=PERSIST_VERSION)
    {
        memset(&persist,0,sizeof(persist));
        persist.dataVersion=PERSIST_VERSION;
    }
}

void savePersistentData()
{
    persist.dataVersion=PERSIST_VERSION;

    File f=LittleFS.open(PERSIST_FILE,"w");

    if(!f)
        return;

    f.write((uint8_t*)&persist,sizeof(PersistentData));

    f.close();
}


void initHealthManager()
{
    memset(&health,0,sizeof(health));

    health.lastHealthUpdate=millis();
}


void updateHealthManager()
{
    if(millis()-health.lastHealthUpdate<1000)
        return;

    health.lastHealthUpdate=millis();

    health.uptime=millis()/1000;

    health.freeHeap=ESP.getFreeHeap();

    health.maxFreeBlock=ESP.getMaxFreeBlockSize();


   
        if(sys.wifiConnected)
        {
            int16_t r = WiFi.RSSI();

            if(r != 31 && r != 0)
                health.rssi = r;
        }
        else
        {
            health.rssi = -127; // یعنی نامعتبر است و متصل نشده است 
        }

    health.wifiConnected=sys.wifiConnected;

    health.apRunning=sys.apEnabled;
    health.updated = true;
    
}


//======================================================
//                  HEALTH REPORT
//======================================================

void printHealthManager()
{
  
    if(!health.updated)
        return;

    health.updated = false;

    if(debug)
        return;

    DBG_PRINT("Heap:");
    DBG_PRINT(health.freeHeap);

    DBG_PRINT(" -RSSI:");
    DBG_PRINT(health.rssi);

    DBG_PRINT(" -Uptime:");
    DBG_PRINT(health.uptime);

    DBG_PRINT(" -WiFi:");
    DBG_PRINT(health.wifiConnected ? "ON" : "OFF");

    // health.apRunning = (WiFi.getMode() & WIFI_AP);
    health.apRunning = sys.apEnabled;
    DBG_PRINT(" -AP:");
    DBG_PRINT(health.apRunning ? "ON" : "OFF");

    DBG_PRINT(" -Boot Counter:");
    DBG_PRINT(persist.bootCounter);
    DBG_PRINT(" -Reset Reason:");
    DBG_PRINTLN(getResetReasonText(boot.lastResetReason));


}



//======================================================
//                  LOGs REPORT
//======================================================

void logSystem(const String &message)
{
    File file = LittleFS.open("/system.log", "a");

    if (!file)
    {
        if (debug)
            DBG_PRINTLN("Logger: open failed");

        return;
    }

    file.print(millis());
    file.print(" | ");
    file.println(message);

    file.close();

            if (debug)
            {
              // DBG_PRINTLN("Logger: entry Saved");
              // DBG_PRINT("msagge=");DBG_PRINTLN(message);
            }
}


void debugReadSystemLog()
{
    File f = LittleFS.open("/system.log", "r");

    if (!f)
    {
        DBG_PRINTLN("Logger: system.log open failed");
        return;
    }

    DBG_PRINTLN("");
    DBG_PRINTLN("========== SYSTEM.LOG ==========");

    while (f.available())
    {
        String line = f.readStringUntil('\n');
        DBG_PRINTLN(line);
    }

    f.close();

    DBG_PRINTLN("======== END SYSTEM.LOG ========");
    DBG_PRINTLN("");
}
//======================================================
//                  TIME INIT
//======================================================

void initTimeManager()
{
    memset(&timeManager, 0, sizeof(timeManager));

    timeManager.initialized = true;
    timeManager.ntpSynced = false;
    timeManager.quality = TIME_UNAVAILABLE;
    timeManager.source = SOURCE_NONE;     // <-- این خط اضافه شد
    timeManager.lastSyncMillis = 0;
    timeManager.lastSyncTime = 0;

    ntpSyncEvent = false;
    ntpSyncEventTime = 0;

    // settimeofday_cb(onTimeSynced);   // <-- ثبت callback واقعی
      #if MODULE_TYPE == MODULE_ESP12F
          settimeofday_cb(onTimeSynced);
      #elif MODULE_TYPE == MODULE_ESP32C3
          sntp_set_time_sync_notification_cb(onTimeSynced);

      #endif
    if (debug)
        DBG_PRINTLN("Time Manager Initialized");
}


void taskTimeManager()
{
    if (!timeManager.initialized)
        return;

    debugPrintTimeStatus();

    // --------------------------------------------------
    // تشخیص جهش ساعت (اصلاح توسط NTP یا Client):
    // اگر ساعت نسبت به millis() بیش از CLOCK_JUMP_THRESHOLD_SEC جابه‌جا شد،
    // Catch-up دوباره اجرا می‌شود تا رله‌های زمان‌محور با برنامه‌ی فعلی هم‌تراز شوند.
    // اجرای مجدد Catch-up فقط وضعیت رله‌های تایمری را تعیین می‌کند.
    // فقط در صورت تغییر واقعی وضعیت رله Trigger می‌شوند.
    // --------------------------------------------------
    {
        static bool          prevValid = false;
        static time_t        prevNow   = 0;
        static unsigned long prevMs    = 0;

        bool nowValid = hasValidClock();

        if (nowValid)
        {
            time_t nowEpoch = time(nullptr);

            if (prevValid)
            {
                long elapsed = (long)((millis() - prevMs) / 1000UL);
                long delta   = (long)(nowEpoch - prevNow);
                long diff    = delta - elapsed;

                if (diff < 0) diff = -diff;

                if (diff > CLOCK_JUMP_THRESHOLD_SEC)
                {
                    schedulerCatchupApplied = false;

                    if (debug)
                    {
                        DBG_PRINT("Clock jump detected (s): ");
                        DBG_PRINTLN(diff);
                    }
                }
            }

            prevNow = nowEpoch;
            prevMs  = millis();
        }

        prevValid = nowValid;
    }

    // --------------------------------------------------
    // Quality بر اساس "کهنگی" آخرین Sync واقعی محاسبه می‌شود
    // (نه فقط اینکه تا حالا sync شده یا نه)
    // از فیلدهای موجود (ntpSynced, lastSyncTime) استفاده شده،
    // هیچ فیلد یا تابع جدیدی اضافه نشده است
    // --------------------------------------------------
 if (timeManager.ntpSynced)
    {

    time_t age = getCurrentTime() - timeManager.lastSyncTime;
    if (age < 0) age = 0;

        bool fresh = ((unsigned long)age <= TIME_STALE_THRESHOLD);
// 14050717
        // if (timeManager.source == SOURCE_NTP)
        //     timeManager.quality = fresh ? TIME_SYNCED : TIME_ESTIMATED;
        // else
        //     timeManager.quality = TIME_ESTIMATED;
            
    timeManager.quality = fresh ? TIME_SYNCED : TIME_ESTIMATED;  // NTP یا Client: هر دو معتبرند
        // 14050717
    }
    else
    {
        timeManager.quality = TIME_UNAVAILABLE;
    }

    // --------------------------------------------------
    // Internet/server unavailable:
    // Wi-Fi association alone is NOT proof of Internet/DNS access.
    // Stop SNTP so lwIP DNS/SNTP timers cannot keep retrying offline.
    // Client/App time synchronization remains independent.
    // --------------------------------------------------
    if (!sys.wifiConnected)  // || !isCloudServerReachableForTimeSync()) شرط اتصال به سرور رو حذف کردم 
    {
        // lastSyncMillis != 0 means configTime/SNTP was started before.
        // Clear it after stopping so a later successful cloud poll can
        // safely start SNTP again, without calling stop on every loop.
        if (timeManager.lastSyncMillis != 0)
        {
#if MODULE_TYPE == MODULE_ESP12F
            sntp_stop();
#elif MODULE_TYPE == MODULE_ESP32C3
            esp_sntp_stop();
#endif
            timeManager.lastSyncMillis = 0;
        }
        return;
    }

    // --------------------------------------------------
    // NTP Sync / Retry
    //
    // برای جلوگیری از تلاش‌های DNS در حالت قطع اینترنت، شروع NTP
    // همچنان به موفقیت آخرین Poll سرور ابری وابسته است.
    //
    // اما اگر اولین NTP ناموفق باشد، دیگر ۶ ساعت منتظر نمی‌مانیم:
    // تا اولین Sync واقعی، هر ۶۰ ثانیه تلاش مجدد می‌کنیم.
    // پس از Sync موفق، فاصله‌ی Sync دوره‌ای ۶ ساعت است.
    // --------------------------------------------------
    const unsigned long ntpElapsed =
        (unsigned long)(millis() - timeManager.lastSyncMillis);

    const bool ntpNeverConfirmed =
        (timeManager.source != SOURCE_NTP);

    const unsigned long ntpRetryInterval =
        ntpNeverConfirmed ? TIME_NTP_RETRY_INTERVAL
                          : TIME_RESYNC_INTERVAL;

    if (timeManager.lastSyncMillis == 0 ||
        ntpElapsed >= ntpRetryInterval)
    {
        if (debug)
        {
            DBG_PRINTLN(ntpNeverConfirmed
                ? "Time NTP retry/start"
                : "Time Periodic NTP Resync");
        }

        syncTime();
    }

    // --------------------------------------------------
    // تایید واقعی Sync
    //
    // ntpSyncEvent فقط توسط onTimeSynced() ست می‌شود، یعنی
    // فقط وقتی که SNTP واقعاً یک پاسخ معتبر از شبکه گرفته باشد.
    // --------------------------------------------------
    if (ntpSyncEvent)
    {
        ntpSyncEvent = false;

        time_t syncedTime = ntpSyncEventTime;

        timeManager.ntpSynced    = true;
        timeManager.source       = SOURCE_NTP;      // <-- این خط اضافه شد        timeManager.quality      = TIME_SYNCED;
        timeManager.quality      = TIME_SYNCED;
        timeManager.lastSyncTime = syncedTime;

        if (debug)
        {
            struct tm* tmInfo = gmtime(&syncedTime);

            DBG_PRINTLN("Time NTP Synced (CONFIRMED by SNTP callback)");
            DBG_PRINT("UTC : ");

            if (tmInfo)
            {
                char buffer[24];
                strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tmInfo);
                DBG_PRINTLN(buffer);
            }

            DBG_PRINTLN("Time Quality : SYNCED");
        }
    }
}


void syncTime()
{
    if (!sys.wifiConnected)
        return;

    configTime(
        0,
        0,
        NTP_SERVERS[0]
        ,NTP_SERVERS[1]
        // ,NTP_SERVERS[2]
    );

    /*
     * نکته مهم:
     *
     * اگر قبلاً زمان معتبر داشته‌ایم، Resync نباید
     * آن را به حالت نامعتبر تبدیل کند.
     *
     * ntpSynced فقط در اولین Sync از false به true
     * تبدیل می‌شود.
     */

    timeManager.lastSyncMillis = millis();

    if (!timeManager.ntpSynced)
        timeManager.quality = TIME_ESTIMATED;

    if (debug && timeManager.quality != TIME_SYNCED)
        DBG_PRINTLN("Time NTP Sync Started");

}

// --------------------------------------------------
// این تابع فقط زمانی صدا زده می‌شود که سیستم واقعاً
// یک پاسخ NTP معتبر از شبکه دریافت کرده باشد.
// fromSntp == true  --> سینک واقعی توسط SNTP
// fromSntp == false --> زمان به‌صورت دستی ست شده (مثلاً از RTC)
// --------------------------------------------------

#if MODULE_TYPE == MODULE_ESP12F

  void onTimeSynced(bool fromSntp)
  {
      if (fromSntp)
      {
          ntpSyncEvent     = true;
          ntpSyncEventTime = time(nullptr);
      }
  }
#elif MODULE_TYPE == MODULE_ESP32C3

  void onTimeSynced(struct timeval *tv)
  {
      ntpSyncEvent     = true;
      ntpSyncEventTime = time(nullptr);
  }
#endif

// فقط برای تست گذاشتم بعدا میشه پاکش کرد 
void debugPrintTimeStatus()
{
    static uint32_t lastPrint = 0;

    if(WifiManagerState != WIFI_CONNECTED)
    return;

    if (millis() - lastPrint < 5000)  // هر 5 ثانیه
        return;

    lastPrint = millis();

    time_t now = getCurrentTime();
    struct tm* tmInfo = gmtime(&now);

    char buffer[24];
    if (tmInfo)
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tmInfo);
    else
        strcpy(buffer, "N/A");

    const char* qualityStr =
        (timeManager.quality == TIME_SYNCED)    ? "SYNCED" :
        (timeManager.quality == TIME_ESTIMATED) ? "ESTIMATED" :
                                                    "UNAVAILABLE";

    const char* sourceStr =
        (timeManager.source == SOURCE_NTP)    ? "NTP" :
        (timeManager.source == SOURCE_CLIENT) ? "CLIENT" :
                                                  "NONE";

    DBG_PRINTF(
        "UTC:%s |Qu:%s |Src:%s |CLKSyn:%s\n",// "[millis=%lu] SysClock UTC: %s | Q: %s | Source: %s | ntpSynced: %s\n",
        buffer, qualityStr, sourceStr,//millis(), buffer, qualityStr, sourceStr,
        timeManager.ntpSynced ? "true" : "false"
    );
}

// ساعت یک‌بار با NTP یا Client معتبر شده و مقدارش منطقی است.
// (ntpSynced در هر دو مسیر NTP و Client برابر true می‌شود.)
// سن آخرین Sync مهم نیست؛ timeManager.quality فقط برای نمایش/تشخیص است
// و مجوز اجرای Scheduler / رله‌های زمان‌محور فقط از این تابع می‌آید.
bool hasValidClock()
{
    return timeManager.ntpSynced && time(nullptr) > 1700000000;
}

// ساعت معتبر است و Catch-up هم تکمیل شده؛ فقط در این حالت وضعیت رله‌های زمان‌محور
// قابل اتکاست (قبل از Catch-up، وضعیت آن‌ها موقتی است). بعد از جهش ساعت تا Catch-up
// جدید دوباره false می‌شود.
bool timeStateReady()
{
    return hasValidClock() && schedulerCatchupApplied;
}

void applyClientTimeSync(time_t clientUtc)
{
 
    // NTP تازه است - اولویت با NTP، Client نادیده گرفته می‌شود
    {
        time_t age = time(nullptr) - timeManager.lastSyncTime;
        if (age < 0) age = 0;

        if (timeManager.source == SOURCE_NTP &&
            timeManager.ntpSynced &&
            (unsigned long)age <= TIME_STALE_THRESHOLD)
            return;
    }

    if (clientUtc <= 1700000000)
        return;   // مقدار نامعتبر

    // Browser status polling must not repeatedly reset the system clock.
    // Correct only when the client clock differs by more than 2 seconds.
    time_t currentUtc = time(nullptr);
    time_t drift = clientUtc - currentUtc;
    if (drift >= -2 && drift <= 2)
        return;

    struct timeval tv = { clientUtc, 0 };
 

    // اگر تنظیم ساعت سیستم شکست بخورد، ساعت را معتبر اعلام نکن
    if (settimeofday(&tv, nullptr) != 0)
    {
        if (debug)
            DBG_PRINTLN("Client time correction failed");

        return;
    }

    timeManager.ntpSynced    = true;
    timeManager.source       = SOURCE_CLIENT;
    timeManager.lastSyncTime = clientUtc;

    if (debug)
    {
        struct tm* tmInfo = gmtime(&clientUtc);
        char buffer[24];

        if (tmInfo)
            strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tmInfo);
        else
            strcpy(buffer, "N/A");

        DBG_PRINT("Time corrected via Client/App fallback - UTC : ");
        DBG_PRINTLN(buffer);
    }
}

void checkClientTimeSync()
{
    if (server.hasArg("utc"))
        applyClientTimeSync((time_t)server.arg("utc").toInt());
}


void handleTimeSync()
{
    checkClientTimeSync();
    server.send(200, "text/plain", "OK");
}

time_t getCurrentTime()
{
    return time(nullptr);
}

//======================================================
//                  sheduler
//======================================================
/*
void taskScheduler()
{
    static unsigned long lastCheck = 0;
    // static bool catchupApplied = false;

    if (millis() - lastCheck < 1000)
        return;

    lastCheck = millis();

    // --------------------------------------------------
    // Policy: UNAVAILABLE = ممنوع بدون استثنا
    // SYNCED و ESTIMATED هر دو مجازند
    // --------------------------------------------------
 if (timeManager.quality == TIME_UNAVAILABLE)
        return;

    if (!schedulerCatchupApplied)// <-- مستقیم به Global رجوع می‌کنه، بدون `static bool` 
    {
        applyCatchupState();
        schedulerCatchupApplied = true;
    }

    time_t now = getCurrentTime();
    struct tm* tmInfo = gmtime(&now);

    if (!tmInfo)
        return;
    // =============================
    // BLINKER PROCESSING
    // =============================

      for (uint8_t i = 0; i < MAX_TIMERS; i++)
      {
          Timer &t = timers[i];

          if (!t.enabled)
              continue;

          if (t.mode != TIMER_MODE_BLINKER)
              continue;

          if (t.blinkStartTime == 0)
              continue;

          processBlinkerTimer(t, now);
      }


  // =============================
  // NORMAL TIMER PROCESSING
  // =============================
      uint8_t  nowHour   = tmInfo->tm_hour;
    uint8_t  nowMinute = tmInfo->tm_min;
    uint8_t  nowWeekday = tmInfo->tm_wday;   // 0=یکشنبه
    uint32_t currentMinuteStamp = now / 60;

    for (uint8_t i = 0; i < MAX_TIMERS; i++)
    {
        Timer &t = timers[i];

        if (!t.enabled)
            continue;

        
        // ---------- پایان بازه ----------
        if (t.activeUntil != 0 && now >= t.activeUntil)
        {
            if (t.relayState)
                relayOff(t.relayIndex, true);
            else
                relayOn(t.relayIndex, true);

            t.activeUntil = 0;

            if (t.type == TIMER_ONCE)
                t.enabled = false;

            continue;
        }

        // ---------- شروع بازه / اکشن لحظه‌ای ----------
        if (t.hour != nowHour || t.minute != nowMinute)
            continue;

        if (t.type == TIMER_WEEKLY &&
            !timerRunsOnWeekday(t, nowWeekday))
            continue;

        if (t.lastTriggerMinute == currentMinuteStamp)
            continue;

        t.lastTriggerMinute = currentMinuteStamp;



        executeAction(t);

        if (t.durationSeconds > 0)
        {
            t.activeUntil = now + ((uint32_t)t.durationSeconds);
        }
        else if (t.type == TIMER_ONCE)
        {
            t.enabled = false;
                        saveTimers();      

        }
    }
}
*/
void taskScheduler()
{
    static unsigned long lastCheck = 0;
    // Catch-up flag is the global schedulerCatchupApplied so that
    // taskTimeManager() can request a new catch-up after a clock jump.

    // Scheduler resolution = 1 second
    if (millis() - lastCheck < 1000)
        return;

    lastCheck = millis();

    // if (timeManager.quality == TIME_UNAVAILABLE)
    //     return;
            //==================================================
            // AUTOMATION TIME GATE
            // Timer / Blinker فقط بعد از Sync معتبر زمان
            // اجازه اجرا دارند
            //==================================================

            if (!hasValidClock())
                return;
    //==================================================
    // CATCH-UP
    //==================================================

    if (!schedulerCatchupApplied)
    {
        applyCatchupState();
        schedulerCatchupApplied = true;
    }

    //==================================================
    // CURRENT TIME
    //==================================================

    time_t now = getCurrentTime();

    struct tm* tmInfo = gmtime(&now);

    if (!tmInfo)
        return;

    uint8_t nowYear =
        tmInfo->tm_year - 100;   // 2026 -> 26

    uint8_t nowMonth =
        tmInfo->tm_mon + 1;      // 1..12

    uint8_t nowDay =
        tmInfo->tm_mday;         // 1..31

    uint8_t nowHour =
        tmInfo->tm_hour;         // 0..23

    uint8_t nowMinute =
        tmInfo->tm_min;          // 0..59

    uint8_t nowSecond =
        tmInfo->tm_sec;          // 0..59

    uint8_t nowWeekday =
        tmInfo->tm_wday;         // 0=Sunday ... 6=Saturday

    //==================================================
    // CURRENT SECOND STAMP
    //==================================================

    uint32_t currentSecondStamp =
        (uint32_t)now;

//==================================================
// BLINKER PROCESSING
//==================================================

for (uint8_t i = 0; i < MAX_TIMERS; i++)
{
    Timer &t = timers[i];

    if (!t.enabled)
        continue;

    if (t.mode != TIMER_MODE_BLINKER)
        continue;

    //================================================
    // ACTIVE BLINKER
    //================================================

    if (t.blinkStartTime != 0)
    {
        processBlinkerTimer(t, now);
        continue;
    }

    //================================================
    // CHECK SCHEDULED START
    //================================================

    bool triggerDate = false;

    // -----------------------------------------------
    // ONCE
    // -----------------------------------------------

    if (t.type == TIMER_ONCE)
    {
        if (t.year == nowYear &&
            t.month == nowMonth &&
            t.day == nowDay)
        {
            triggerDate = true;
        }
    }

    // -----------------------------------------------
    // DAILY
    // -----------------------------------------------

    else if (t.type == TIMER_DAILY)
    {
        triggerDate = true;
    }

    // -----------------------------------------------
    // WEEKLY
    // -----------------------------------------------

    else if (t.type == TIMER_WEEKLY)
    {
        if (timerRunsOnWeekday(t, nowWeekday))
        {
            triggerDate = true;
        }
    }

    if (!triggerDate)
        continue;

    //================================================
    // EXACT TIME
    //================================================

    if (t.hour   != nowHour   ||
        t.minute != nowMinute ||
        t.second != nowSecond)
    {
        continue;
    }

    //================================================
    // PREVENT DOUBLE TRIGGER
    //================================================

    if (t.lastTriggerSecond == currentSecondStamp)
        continue;

    //================================================
    // CHECK CANCELLED OCCURRENCE
    //================================================

    if (t.cancelledOccurrenceSecond ==
        currentSecondStamp)
    {
        t.lastTriggerSecond =
            currentSecondStamp;

        continue;
    }

    //================================================
    // MARK TRIGGER
    //================================================

    t.lastTriggerSecond =
        currentSecondStamp;

    //================================================
    // START BLINKER
    //================================================

    startBlinkerTimer(t, now);
}


    //==================================================
    // NORMAL TIMER PROCESSING
    //==================================================

    for (uint8_t i = 0; i < MAX_TIMERS; i++)
    {
        Timer &t = timers[i];

        if (!t.enabled)
            continue;

        // Blinker is handled separately above
        if (t.mode == TIMER_MODE_BLINKER)
            continue;

        //================================================
        // ACTIVE TIMER END
        //================================================

        if (t.activeUntil != 0 &&
    now >= t.activeUntil)
        {
            if (t.relayState)
                relayOff(t.relayIndex, true);
            else
                relayOn(t.relayIndex, true);

            t.activeUntil = 0;

            if (t.type == TIMER_ONCE)
                t.enabled = false;

            continue;
        }

        //================================================
        // TYPE SPECIFIC DATE CHECK
        //================================================

        bool triggerDate = false;

        // -----------------------------------------------
        // ONCE
        // -----------------------------------------------

        if (t.type == TIMER_ONCE)
        {
            if (t.year == nowYear &&
                t.month == nowMonth &&
                t.day == nowDay)
            {
                triggerDate = true;
            }
        }

        // -----------------------------------------------
        // DAILY
        // -----------------------------------------------

        else if (t.type == TIMER_DAILY)
        {
            // Every day
            triggerDate = true;
        }

        // -----------------------------------------------
        // WEEKLY
        // -----------------------------------------------
else if (t.type == TIMER_WEEKLY)
        {
            if (timerRunsOnWeekday(t, nowWeekday))
            {
                triggerDate = true;
            }
        }

        if (!triggerDate)
            continue;

        //================================================
        // TIME CHECK
        //================================================

        if (t.hour   != nowHour   ||
            t.minute != nowMinute ||
            t.second != nowSecond)
        {
            continue;
        }

        //================================================
        // PREVENT DOUBLE TRIGGER
        //================================================

        if (t.lastTriggerSecond == currentSecondStamp)
            continue;
        if (t.cancelledOccurrenceSecond ==
            currentSecondStamp)
        {
            t.lastTriggerSecond =
                currentSecondStamp;

            continue;
        }
        t.lastTriggerSecond =
            currentSecondStamp;

        //================================================
        // EXECUTION
        //================================================

        if (t.mode == TIMER_MODE_BLINKER)
        {
            startBlinkerTimer(t, now);
        }
        else
        {
            executeAction(t);

            if (t.durationSeconds > 0)
            {
                t.activeUntil =
                    now +
                    ((uint32_t)t.durationSeconds);
            }
            else if (t.type == TIMER_ONCE)
            {
                 t.enabled = false;
                
    // ONCE + duration=0 => Persistent Action
    // دیگر وضعیت رله وابسته به زمان نیست.

                relayTimeDependent[t.relayIndex] = false;
                
    // این وضعیت باید همین الان روی Flash ذخیره شود
    // تا حتی Reset بلافاصله بعد از اجرای Timer هم امن باشد.
    saveRelayState();
    // برای حفظ هماهنگی مکانیزم ذخیره رله
    sys.relayNeedSave = false;
            }
        }
    }
}

void executeAction(Timer &t)
{
    if (t.relayState)
        relayOn(t.relayIndex, true);
    else
        relayOff(t.relayIndex, true);
}


//--------------------------------

time_t timerDateTimeToEpoch(
    uint8_t year,
    uint8_t month,
    uint8_t day,
    uint8_t hour,
    uint8_t minute,
    uint8_t second
)
{
    struct tm tmValue;

    memset(&tmValue, 0, sizeof(tmValue));

    tmValue.tm_year = 100 + year;   // 2000 + year
    tmValue.tm_mon  = month - 1;
    tmValue.tm_mday = day;
    tmValue.tm_hour = hour;
    tmValue.tm_min  = minute;
    tmValue.tm_sec  = second;

    return mktime(&tmValue);
}
/*
void applyCatchupState()
{
    time_t now = getCurrentTime();
    struct tm* tmInfo = gmtime(&now);

    if (!tmInfo)
        return;

    time_t midnightToday = now - (tmInfo->tm_hour * 3600 +
                                   tmInfo->tm_min  * 60 +
                                   tmInfo->tm_sec);

    uint8_t todayWeekday = tmInfo->tm_wday;
    uint8_t yestWeekday  = (todayWeekday + 6) % 7;

    for (uint8_t relay = 0; relay < TOUCH_COUNT; relay++)
    {
        bool   found = false;
        bool   bestState = false;
        time_t bestAbs = 0;

        for (uint8_t i = 0; i < MAX_TIMERS; i++)
        {
            Timer &t = timers[i];

            if (!t.enabled || t.relayIndex != relay || t.type == TIMER_ONCE)
                continue;

            // ---------- نمونه‌ی «امروز» ----------
            if (t.type != TIMER_WEEKLY || timerRunsOnWeekday(t, todayWeekday))
            {
                time_t startAbs = midnightToday + t.hour * 3600 + t.minute * 60;

                if (startAbs <= now && startAbs > bestAbs)
                {
                    bestAbs = startAbs;
                    bestState = t.relayState;
                    found = true;
                }

                if (t.durationSeconds > 0)
                {
                    time_t endAbs = startAbs + (time_t)t.durationSeconds;
                    if (startAbs <= now && startAbs > bestAbs)
                    {
                        bestAbs = startAbs;
                        bestState = t.relayState;
                        found = true;
                    }
                }
            }

            // ---------- نمونه‌ی «دیروز» (فقط بازه‌هایی که ممکنه به امروز کشیده باشن) ----------
            if (t.durationSeconds > 0 &&
                (t.type != TIMER_WEEKLY || timerRunsOnWeekday(t, yestWeekday)))
            {
                time_t startAbsY = midnightToday - 86400 + t.hour * 3600 + t.minute * 60;
                time_t endAbsY   = startAbsY + (time_t)t.durationSeconds;

                if (startAbsY <= now && startAbsY > bestAbs)
                {
                    bestAbs = startAbsY;
                    bestState = t.relayState;
                    found = true;
                }

                if (endAbsY <= now && endAbsY > bestAbs)
                {
                    bestAbs = endAbsY;
                    bestState = !t.relayState;
                    found = true;
                }
            }
        }

        if (found)
        {
            if (bestState)
                relayOn(relay, true);
            else
                relayOff(relay, true);
        }
    }

    // --------------------------------------------------
    // اجرای دیرهنگام ONCE
    // --------------------------------------------------
    uint16_t nowMinutesOfDay = tmInfo->tm_hour * 60 + tmInfo->tm_min;
  for (uint8_t i = 0; i < MAX_TIMERS; i++)
    {
        Timer &t = timers[i];

        if (!t.enabled || t.type != TIMER_ONCE)
            continue;

        uint16_t startMin = t.hour * 60 + t.minute;

        if (startMin <= nowMinutesOfDay)
        {
            uint16_t endMin = startMin + t.durationSeconds;

            if (t.durationSeconds > 0 && endMin > nowMinutesOfDay)
            {
                executeAction(t);
                t.activeUntil = midnightToday + (time_t)endMin * 60;
            }
            else
            {
                if (debug)
                    DBG_PRINTLN("Timer ONCE: Delayed catch-up execution (window fully expired)");

                if (t.durationSeconds > 0)
                {
                    // بازه کاملاً گذشته - فقط وضعیت نهایی را اعمال کن، نه شروع را
                    if (t.relayState) relayOff(t.relayIndex, true);
                    else               relayOn(t.relayIndex, true);
                }
                else
                {
                    // بدون duration بود؛ یک اکشن لحظه‌ای تنها - دیرهنگام اجرا کن
                    executeAction(t);
                }

                t.enabled = false;
                saveTimers();
            }

        }
    }
}
*/
void applyCatchupState()
{
    time_t now = getCurrentTime();

    struct tm* tmInfo = gmtime(&now);

    if (!tmInfo)
        return;

    //==================================================
    // CURRENT DATE / TIME
    //==================================================

    uint8_t nowYear =
        tmInfo->tm_year - 100;

    uint8_t nowMonth =
        tmInfo->tm_mon + 1;

    uint8_t nowDay =
        tmInfo->tm_mday;

    uint8_t nowWeekday =
        tmInfo->tm_wday;

    time_t midnightToday =
        now -
        (tmInfo->tm_hour * 3600 +
         tmInfo->tm_min  * 60 +
         tmInfo->tm_sec);

    //==================================================
    // 1. NORMAL DAILY / WEEKLY TIMERS
    //==================================================

    for (uint8_t relay = 0;
         relay < TOUCH_COUNT;
         relay++)
    {
        bool   found = false;
        bool   bestState = false;
        time_t bestAbs = 0;

        for (uint8_t i = 0;
             i < MAX_TIMERS;
             i++)
        {
            Timer &t = timers[i];

            if (!t.enabled)
                continue;

            if (t.mode == TIMER_MODE_BLINKER)
                continue;

            if (t.relayIndex != relay)
                continue;

            if (t.type == TIMER_ONCE)
                continue;

            //================================================
            // TODAY
            //================================================

            bool validToday = false;

            if (t.type == TIMER_DAILY)
            {
                validToday = true;
            }
            else if (t.type == TIMER_WEEKLY)
            {
                validToday =
                    timerRunsOnWeekday(t, nowWeekday);
            }

            if (validToday)
            {
                time_t startAbs =
                    midnightToday +
                    t.hour * 3600 +
                    t.minute * 60 +
                    t.second;

                if (isOccurrenceCancelled(t, startAbs))
                    continue;

                if (startAbs <= now &&
                    startAbs > bestAbs)
                {
                    bestAbs = startAbs;
                    bestState = t.relayState;
                    found = true;
                }

                if (t.durationSeconds > 0)
                {
                    time_t endAbs =
                        startAbs +
                        (time_t)t.durationSeconds;

                    if (endAbs <= now &&
                        endAbs > bestAbs)
                    {
                        bestAbs = endAbs;
                        bestState = !t.relayState;
                        found = true;
                    }
                    else if (startAbs <= now &&
                             now < endAbs)
                    {
                        bestAbs = startAbs;
                        bestState = t.relayState;
                        found = true;
                    }
                }
            }
            //================================================
            // YESTERDAY
            //================================================

            if (t.durationSeconds > 0)
            {
                uint8_t yesterdayWeekday =
                    (nowWeekday + 6) % 7;

                bool validYesterday = false;

                if (t.type == TIMER_DAILY)
                {
                    validYesterday = true;
                }
                else if (t.type == TIMER_WEEKLY)
                {
                    validYesterday =
                        timerRunsOnWeekday(t, yesterdayWeekday);
                }

                if (validYesterday)
                {
                    time_t startAbsY =
                        midnightToday -
                        86400 +
                        t.hour * 3600 +
                        t.minute * 60 +
                        t.second;

                      if (isOccurrenceCancelled(t, startAbsY))
                          continue;

                    time_t endAbsY =
                        startAbsY +
                        (time_t)t.durationSeconds;

                    if (startAbsY <= now &&
                        now < endAbsY)
                    {
                        if (startAbsY > bestAbs)
                        {
                            bestAbs = startAbsY;
                            bestState = t.relayState;
                            found = true;
                        }
                    }
                    else if (endAbsY <= now &&
                             endAbsY > bestAbs)
                    {
                        bestAbs = endAbsY;
                        bestState = !t.relayState;
                        found = true;
                    }
                }
            }
        }

        if (found)
        {
            if (bestState)
                relayOn(relay, true);
            else
                relayOff(relay, true);
        }
    }

    //==================================================
    // 2. NORMAL ONCE TIMERS
    //==================================================

    for (uint8_t i = 0;
         i < MAX_TIMERS;
         i++)
    {
        Timer &t = timers[i];

        if (!t.enabled)
            continue;

        if (t.mode == TIMER_MODE_BLINKER)
            continue;

        if (t.type != TIMER_ONCE)
            continue;

        time_t startAbs =
            timerDateTimeToEpoch(
                t.year,
                t.month,
                t.day,
                t.hour,
                t.minute,
                t.second
            );
      if (isOccurrenceCancelled(t, startAbs))
          continue;

        // Timer has not started yet
        if (startAbs > now)
            continue;

        //================================================
        // NO DURATION
        //================================================

        if (t.durationSeconds == 0)
          {
              if (debug)
              {
                  DBG_PRINTLN(
                      "Timer ONCE: Delayed catch-up execution"
                  );
              }

              executeAction(t);

              t.enabled = false;

              // ONCE + duration=0 => Persistent Action
              // وضعیت نهایی رله باید بعد از ریست هم قابل Restore باشد.
              relayTimeDependent[t.relayIndex] = false;
              
              // وضعیت نهایی را فوری ذخیره کن
              saveRelayState();
              
              sys.relayNeedSave = false;

              saveTimers();

              continue;
          }

        //================================================
        // TIMER HAS A DURATION
        //================================================

        time_t endAbs =
            startAbs +
            (time_t)t.durationSeconds;

        // Timer is still active
        if (now < endAbs)
        {
            executeAction(t);

            t.activeUntil =
                endAbs;

            continue;
        }

        //================================================
        // TIMER FULLY EXPIRED
        //================================================

        if (debug)
        {
            DBG_PRINTLN(
                "Timer ONCE: Delayed catch-up execution "
                "(window fully expired)"
            );
        }

        if (t.relayState)
            relayOff(t.relayIndex, true);
        else
            relayOn(t.relayIndex, true);

        t.activeUntil = 0;
        t.enabled = false;

        saveTimers();
    }
    //==================================================
    // 3. BLINKER RECOVERY
    //==================================================

    for (uint8_t i = 0;
         i < MAX_TIMERS;
         i++)
    {
        Timer &t = timers[i];

        if (!t.enabled)
            continue;

        if (t.mode != TIMER_MODE_BLINKER)
            continue;

        //================================================
        // CONFIG VALIDATION
        //================================================

        if (t.blinkOnSeconds < BLINK_MIN_SECONDS ||
            t.blinkOnSeconds > BLINK_MAX_SECONDS)
            continue;

        if (t.blinkOffSeconds < BLINK_MIN_SECONDS ||
            t.blinkOffSeconds > BLINK_MAX_SECONDS)
            continue;

        if (t.blinkerCount == 0)
            continue;

        //================================================
        // OCCURRENCE START
        //================================================

        time_t occurrenceStart =
            t.blinkerOccurrenceStart;

        //================================================
        // IF OCCURRENCE WAS NOT PERSISTED,
        // FIND IT FROM SCHEDULE
        //================================================

        if (occurrenceStart == 0)
        {
            if (t.type == TIMER_ONCE)
            {
                occurrenceStart =
                    timerDateTimeToEpoch(
                        t.year,
                        t.month,
                        t.day,
                        t.hour,
                        t.minute,
                        t.second
                    );
            }
            else
            {
                // DAILY / WEEKLY:
                // First check today's occurrence.

                bool validToday = false;

                if (t.type == TIMER_DAILY)
                {
                    validToday = true;
                }
                else if (t.type == TIMER_WEEKLY)
                {
                    validToday =
                        timerRunsOnWeekday(t, nowWeekday);
                }

                if (validToday)
                {
                    occurrenceStart =
                        midnightToday +
                        t.hour * 3600 +
                        t.minute * 60 +
                        t.second;

                    if (occurrenceStart > now)
                        occurrenceStart = 0;
                }

                // If today's occurrence is not valid,
                // check yesterday.

                if (occurrenceStart == 0)
                {
                    uint8_t yesterdayWeekday =
                        (nowWeekday + 6) % 7;

                    bool validYesterday = false;

                    if (t.type == TIMER_DAILY)
                    {
                        validYesterday = true;
                    }
                    else if (t.type == TIMER_WEEKLY)
                    {
                        validYesterday =
                            timerRunsOnWeekday(t, yesterdayWeekday);
                    }

                    if (validYesterday)
                    {
                        occurrenceStart =
                            midnightToday -
                            86400 +
                            t.hour * 3600 +
                            t.minute * 60 +
                            t.second;

                        if (occurrenceStart > now)
                            occurrenceStart = 0;
                    }
                }
            }
        }

        if (occurrenceStart == 0)
            continue;

        //================================================
        // TOTAL BLINKER DURATION
        //================================================

        uint32_t cycleSeconds =
            t.blinkOnSeconds +
            t.blinkOffSeconds;

        uint32_t totalSeconds = timerBlinkFiniteDurationSeconds(
            t.blinkOnSeconds, t.blinkOffSeconds, t.blinkerCount);
      if (cycleSeconds == 0 ||
            (t.blinkerCount != 255 && totalSeconds == 0))
            continue;
        //================================================
        // CHECK CANCELLED OCCURRENCE
        //================================================    
          if (isOccurrenceCancelled(t, occurrenceStart))
          {
              t.blinkStartTime = 0;
              t.blinkEndTime = 0;
              t.blinkNextToggle = 0;
              t.blinkState = false;
              t.blinkerExecutedCount = 0;
              t.blinkerOccurrenceStart = 0;

              saveTimers();

              continue;
          }
        //================================================
        // CALCULATE ELAPSED TIME
        //================================================

        time_t elapsed =
            now - occurrenceStart;

        if (elapsed < 0)
            continue;

        //================================================
        // OCCURRENCE ALREADY FINISHED
        //================================================

        if (t.blinkerCount != 255 &&
            (uint32_t)elapsed >= totalSeconds)
        {
            relayOff(t.relayIndex, true);

            t.blinkStartTime = 0;

            t.blinkEndTime = 0;

            t.blinkNextToggle = 0;

            t.blinkState = false;

            t.blinkerExecutedCount = 0;

            t.blinkerOccurrenceStart = 0;

            // ONCE must be disabled
            if (t.type == TIMER_ONCE)
                t.enabled = false;

            saveTimers();

            continue;
        }

        //================================================
        // RECOVER ACTIVE BLINKER
        //================================================

        t.blinkStartTime =
            occurrenceStart;

        t.blinkEndTime =
            (t.blinkerCount == 255) ? 0 :
            occurrenceStart + totalSeconds;

        t.blinkerOccurrenceStart =
            occurrenceStart;

        bool currentState;
        time_t nextToggle;

        if (calculateBlinkerState(
                t,
                now,
                currentState,
                nextToggle))
        {
            t.blinkState =
                currentState;

            t.blinkNextToggle =
                nextToggle;

            if (currentState)
                relayOn(t.relayIndex, true);
            else
                relayOff(t.relayIndex, true);
        }

        // Persist recovered runtime state
        saveTimers();
    }
}

void loadTimers()
{
    File f = LittleFS.open("/timers.dat", "r");

    if (!f)
    {
        memset(timers, 0, sizeof(timers));
        return;
    }

    const size_t fileSize = f.size();
    if (fileSize == sizeof(timers))
    {
        f.read((uint8_t *)timers, sizeof(timers));
    }
    else if (fileSize == sizeof(Timer) * 16U)
    {
        // Migrate the previous 16-slot layout to two slots per relay.
        // Keep the first two slots of each relay and discard excess legacy slots.
        Timer oldTimers[16];
        if (f.read((uint8_t *)oldTimers, sizeof(oldTimers)) == sizeof(oldTimers))
        {
            memset(timers, 0, sizeof(timers));
            const uint8_t oldSlotsPerRelay = (TOUCH_COUNT > 0) ? (16 / TOUCH_COUNT) : 0;
            for (uint8_t relay = 0; relay < TOUCH_COUNT; relay++)
            {
                for (uint8_t timerSlot = 0; timerSlot < 2; timerSlot++)
                {
                    if (timerSlot >= oldSlotsPerRelay)
                        continue;
                    const uint8_t oldIndex = relay * oldSlotsPerRelay + timerSlot;
                    const uint8_t newIndex = relay * 2 + timerSlot;
                    timers[newIndex] = oldTimers[oldIndex];
                    if (timers[newIndex].enabled)
                        timers[newIndex].relayIndex = relay;
                    timers[newIndex].timerId = (uint8_t)((relay << 4) | timerSlot);
                }
            }
            f.close();
            saveTimers();
            return;
        }
        memset(timers, 0, sizeof(timers));
    }
    else
    {
        memset(timers, 0, sizeof(timers));
    }

    f.close();

    // Normalize IDs to the new relay/timer-slot addressing scheme.
    for (uint8_t i = 0; i < MAX_TIMERS; i++)
    {
        const uint8_t relay = i / 2;
        const uint8_t timerSlot = i % 2;
        if (!timers[i].enabled)
            timers[i].relayIndex = relay;
        timers[i].timerId = (uint8_t)(((timers[i].relayIndex & 0x0F) << 4) | timerSlot);
    }
}

void saveTimers()
{
    File f = LittleFS.open("/timers.dat", "w");

    if (!f)
        return;

    f.write((uint8_t *)timers, sizeof(timers));

    f.close();
}
//======================================================
//                  BLINKER
//======================================================
bool calculateBlinkerState(
    Timer &t,
    time_t now,
    bool &state,
    time_t &nextToggle
)
{
    if (t.mode != TIMER_MODE_BLINKER)
        return false;

    if (t.blinkStartTime == 0)
        return false;

    if (t.blinkOnSeconds < BLINK_MIN_SECONDS ||
        t.blinkOnSeconds > BLINK_MAX_SECONDS)
        return false;

    if (t.blinkOffSeconds < BLINK_MIN_SECONDS ||
        t.blinkOffSeconds > BLINK_MAX_SECONDS)
        return false;

    if (t.blinkerCount == 0)
        return false;

    uint32_t cycleSeconds =
        t.blinkOnSeconds + t.blinkOffSeconds;

    if (cycleSeconds == 0)
        return false;

    uint32_t totalSeconds = timerBlinkFiniteDurationSeconds(
        t.blinkOnSeconds, t.blinkOffSeconds, t.blinkerCount);

    time_t elapsed = now - t.blinkStartTime;

    if (elapsed < 0)
        return false;

    if (t.blinkerCount != 255 &&
        (uint32_t)elapsed >= totalSeconds)
        return false;

    uint32_t position =
        (uint32_t)elapsed % cycleSeconds;

    if (position < t.blinkOnSeconds)
    {
        state = true;

        uint32_t remaining =
            t.blinkOnSeconds - position;

        nextToggle = now + remaining;
    }
    else
    {
        state = false;

        uint32_t remaining =
            cycleSeconds - position;

        nextToggle = now + remaining;
    }

    return true;
}

void processBlinkerTimer(Timer &t, time_t now)
{
    if (!t.enabled)
        return;

    if (t.mode != TIMER_MODE_BLINKER)
        return;

    if (t.blinkStartTime == 0)
        return;

    uint32_t cycleSeconds =
        t.blinkOnSeconds + t.blinkOffSeconds;

    if (cycleSeconds == 0)
        return;

    uint32_t totalSeconds = timerBlinkFiniteDurationSeconds(
        t.blinkOnSeconds, t.blinkOffSeconds, t.blinkerCount);

    time_t elapsed =
        now - t.blinkStartTime;

    if (elapsed < 0)
        return;

    // Count 255 means repeat forever; 1..254 are finite repetitions.
    if (t.blinkerCount != 255 &&
        (uint32_t)elapsed >= totalSeconds)
    {
        relayOff(t.relayIndex, true);

        t.blinkStartTime  = 0;
        t.blinkEndTime    = 0;
        t.blinkNextToggle = 0;
        t.blinkState      = false;
        t.blinkerExecutedCount = 0;
        t.blinkerOccurrenceStart = 0;

        if (t.type == TIMER_ONCE)
            t.enabled = false;

        return;
    }

    // Nothing to change yet
    if (t.blinkNextToggle != 0 &&
        now < t.blinkNextToggle)
        return;

    bool newState;
    time_t nextToggle;
if (!calculateBlinkerState(
            t,
            now,
            newState,
            nextToggle))
    {
        relayOff(t.relayIndex, true);
        return;
    }

    t.blinkState      = newState;
    t.blinkNextToggle = nextToggle;

    if (newState)
        relayOn(t.relayIndex, true);
    else
        relayOff(t.relayIndex, true);
}

void startBlinkerTimer(Timer &t, time_t startTime)
{
    if (!t.enabled)
        return;

    if (t.mode != TIMER_MODE_BLINKER)
        return;

    if (t.blinkOnSeconds < BLINK_MIN_SECONDS ||
        t.blinkOnSeconds > BLINK_MAX_SECONDS)
        return;

    if (t.blinkOffSeconds < BLINK_MIN_SECONDS ||
        t.blinkOffSeconds > BLINK_MAX_SECONDS)
        return;

    if (t.blinkerCount == 0)
        return;

    uint32_t cycleSeconds =
        t.blinkOnSeconds + t.blinkOffSeconds;

    uint32_t totalSeconds = timerBlinkFiniteDurationSeconds(
        t.blinkOnSeconds, t.blinkOffSeconds, t.blinkerCount);

    t.blinkStartTime =
    startTime;

    t.blinkerOccurrenceStart =
    startTime;

    t.blinkEndTime =
        (t.blinkerCount == 255) ? 0 : startTime + totalSeconds;

    t.blinkNextToggle =
        startTime + t.blinkOnSeconds;

    t.blinkState = true;

    t.blinkerExecutedCount = 0;

    relayOn(t.relayIndex, true);

              saveTimers();

    if (debug)
    {
        DBG_PRINT("Blinker START - Relay: ");
        DBG_PRINT(t.relayIndex);
        DBG_PRINT(" Count: ");
        DBG_PRINT(t.blinkerCount);
        DBG_PRINT(" ON: ");
        DBG_PRINT(t.blinkOnSeconds);
        DBG_PRINT(" OFF: ");
        DBG_PRINT(t.blinkOffSeconds);
        DBG_PRINT(" Total: ");
        DBG_PRINTLN(totalSeconds);
    }
}

