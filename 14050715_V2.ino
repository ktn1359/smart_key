//14050715 1000

//======================================================
//                  MODULE TYPE// کدام ماژول روی برد است ؟
//======================================================

#define MODULE_ESP12F    1
#define MODULE_ESP32C3   2

#define MODULE_TYPE      MODULE_ESP12F   // روز مهاجرت، فقط همین خط رو به MODULE_ESP32C3 عوض کنم


#define PERSIST_VERSION   1
#define PERSIST_FILE "/runtime.dat"


//========================//1==============================
//                      INCLUDE
//======================================================

#if MODULE_TYPE == MODULE_ESP12F

    #include <ESP8266WiFi.h>
    #include <ESP8266WebServer.h>
    #include <ESP8266mDNS.h>
    #include <coredecls.h>      // settimeofday_cb()

    typedef ESP8266WebServer WebServerType;

#elif MODULE_TYPE == MODULE_ESP32C3

    #include <WiFi.h>
    #include <WebServer.h>
    #include <ESPmDNS.h>
    #include <esp_sntp.h>       // sntp_set_time_sync_notification_cb()

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
#define MAX_TIMERS 16 // تعداد تایمرها
#define TIME_RESYNC_INTERVAL 30000UL//21600000UL   // 6 hours
#define TIME_STALE_THRESHOLD 60UL//86400UL   // مثلا 24 ساعت - عدد دلخواه پروژه

#define TIMER_MODE_NORMAL   0
#define TIMER_MODE_BLINKER  1

#define BLINK_MIN_SECONDS   5UL     // MIN 5 SECONDS
#define BLINK_MAX_SECONDS   43200UL //MAX  12 HOURS

const char* NTP_SERVERS[] =
{
    "pool.ntp.org",
    "time.nist.gov",
    "time.google.com"
};

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
//======================================================
//              Rule manager 
//======================================================
#define RULES_FILE "/rules.dat"
#define MAX_RULES 16 // تعداد رولها 
#define MAX_PENDING_ACTIONS 8 // حداکثر تعداد رولهای در صف تاخیر

#define MAX_PENDING_ACTIONS 8   // حداکثر تعداد تاخیرها در رولها
#define MIN_RULE_DELAY_SECONDS 15UL//60UL // حداقل میزان تاخیر برای رول ها فعلا 1 دقیقه  
#define MAX_RULE_DELAY_SECONDS 86400UL // حداکثر میزان تاخیر برای رولها فعلا 24 ساعت

enum RuleTriggerType
{
    RULE_TRIGGER_NONE = 0,
    RULE_TRIGGER_RELAY_STATE,
    RULE_TRIGGER_SCENARIO       // با رسیدن یک پیام SCENE (از شبکه‌ی محلی یا سرور) فعال می‌شود
};
enum RuleConditionType
{
    RULE_CONDITION_NONE = 0,
    RULE_CONDITION_RELAY_STATE
};
enum RuleActionType
{
    RULE_ACTION_NONE = 0,
    RULE_ACTION_RELAY_ON,
    RULE_ACTION_RELAY_OFF,
    RULE_ACTION_RELAY_TOGGLE
};

struct Rule
{
    bool enabled;

    uint8_t triggerType;
    uint8_t triggerSource;
    bool    triggerValue;
    bool    lastTriggerState;
    uint8_t triggerScenarioId;   // فقط برای RULE_TRIGGER_SCENARIO
    uint32_t lastScenarioEvent;  // آخرین scenarioEventCounter که این Rule بهش واکنش نشون داده (جلوگیری از اجرای تکراری)

    uint8_t conditionType;
    uint8_t conditionSource;
    bool    conditionValue;

    uint8_t actionType;
    uint8_t actionTarget;

    bool    actionIsRemote;      // true = actionTarget روی یک Peer اجرا شود، نه محلی
    uint8_t actionPeerIndex;     // اندیس داخل peers[] (فقط وقتی actionIsRemote=true)

    uint32_t actionDelaySeconds;
};
Rule rules[MAX_RULES];

struct PendingRuleAction
{
    bool active;

    uint8_t ruleIndex;
    uint8_t actionType;
    uint8_t actionTarget;

    bool    actionIsRemote;
    uint8_t actionPeerIndex;

    uint32_t executeAt;
};
PendingRuleAction pendingRuleActions[MAX_PENDING_ACTIONS];
//======================================================
//                  FIRMWARE VERSION
//======================================================

#define FW_VERSION_MAJOR     1
#define FW_VERSION_MINOR     0
#define FW_VERSION_BUILD     0

#define FW_BUILD_DATE        14050715
#define FW_BUILD_TIME        1600

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

#define TOUCH_COUNT        4 // تعداد رله و تاچهای هر دستگاه 
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
#define BOOT_RELAY_MAX_WAIT_MS    30000UL// حداکثر زمان توقف برای رله ها در زمان بوت شدن

bool relayTimeDependent[TOUCH_COUNT] = {false};
// false = منبعش تاچ/اپ/بک‌اند بوده (بدون نیاز به زمان)
// true  = منبعش Timer/Scheduler بوده (نیاز به زمان معتبر دارد)

bool relayRestoreOnBoot[TOUCH_COUNT] = {true, true, true, true};
// true  = بعد از بوت، آخرین وضعیت/تایمرهای این رله بازیابی می‌شود (رفتار قبلی)
// false = این رله صرف‌نظر از وضعیت قبلی یا تایمرهای فعال، همیشه بعد از بوت خاموش می‌ماند

//------------------------------------------------------
//  PeerLink (ارتباط محلی بین دستگاه‌ها) — تعریف کامل توی تب PeerLink.ino
//  این چند خط این‌جان چون متغیر Global (برخلاف تابع) باید قبل از محل
//  استفاده‌ش تعریف شده باشه، و scanInputs همین‌جا، توی همین تب، استفاده‌شون می‌کنه.
//------------------------------------------------------
bool    touchFiresScenario[TOUCH_COUNT] = {false};
uint8_t touchScenarioId[TOUCH_COUNT]    = {0};
// اگه touchFiresScenario[i]==true باشه، لمس تاچ شماره‌ی i به‌جای
// روشن/خاموش‌کردن رله‌ی خودش، سناریوی touchScenarioId[i] رو اجرا می‌کنه.

uint32_t scenarioEventCounter = 0;   // همین دلیل: checkRuleTrigger همین‌جا توی این تب ازش استفاده می‌کنه
uint8_t  lastFiredScenarioId  = 0;

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
void handleRoot();
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
void taskInputs();
void taskRelaySave();
void taskStatus();
void runSystem();
void startSystem();
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

void taskScheduler();
void handleSetTimer();
void loadTimers();
void saveTimers();
void handleGetTimer();

//---------------- syne Time& Scheduler ----------------

void processBlinkerTimer(Timer &t, time_t now);

//---------------- Rule Manager ----------------

bool checkRuleTrigger(Rule &r);
bool checkRuleCondition(Rule &r);

// پیش‌اعلان دستی برای دو تابعی که توی تب PeerLink.ino تعریف شدن ولی
// این تب (که قبل از PeerLink.ino قرار می‌گیره) صداشون می‌زنه. چون خودِ
// PeerLink.ino هم یه پیش‌اعلان محلی برای sendCommandToPeer داره، تولید
// خودکار Prototype براش رد می‌شه و این تب دیگه نمی‌بینتش — برای همین
// این‌جا هم صریح اعلامش می‌کنیم.
void fireScenarioLocal(uint8_t sceneId);
bool sendCommandToPeer(uint8_t peerIndex, uint8_t relay, bool desiredOn);
void executeRuleAction(Rule &r);
void executeRule(Rule &r, uint8_t ruleIndex);
void taskRuleManager();
void processPendingRuleActions();
void handleSetRule();
void handleGetRule();
void cancelRuleAction(uint8_t ruleIndex);




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

    size_t expectedSize = sizeof(relayState) + sizeof(relayTimeDependent) + sizeof(relayRestoreOnBoot);

    if (!f)
    {
        memset(relayState, 0, sizeof(relayState));
        memset(relayTimeDependent, 0, sizeof(relayTimeDependent));
       for (byte i = 0; i < TOUCH_COUNT; i++)
            relayRestoreOnBoot[i] = true;
        return;
    }

    if (f.size() == expectedSize)
    {
        f.read((uint8_t *)relayState, sizeof(relayState));
        f.read((uint8_t *)relayTimeDependent, sizeof(relayTimeDependent));
        f.read((uint8_t *)relayRestoreOnBoot, sizeof(relayRestoreOnBoot));
    }
    else
    {
        // فایل مربوط به نسخه‌ی قدیمی‌تر فرموره (بدون relayRestoreOnBoot) یا خرابه؛
        // یک‌بار با پیش‌فرض «بازیابی فعال» صفر می‌شه (رفتار قبلی حفظ می‌شه).
        memset(relayState, 0, sizeof(relayState));
        memset(relayTimeDependent, 0, sizeof(relayTimeDependent));
        for (byte i = 0; i < TOUCH_COUNT; i++)
            relayRestoreOnBoot[i] = true;
    }

    f.close();
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

    f.write((uint8_t *)relayState, sizeof(relayState));
    f.write((uint8_t *)relayTimeDependent, sizeof(relayTimeDependent));
    f.write((uint8_t *)relayRestoreOnBoot, sizeof(relayRestoreOnBoot));
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

    byte got = Wire.requestFrom(MCP_ADDR, (uint8_t)1);

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
                timeManager.quality != TIME_SYNCED)
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
                !(t.weekMask &
                  (1 << occInfo->tm_wday)))
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
void relayOn(byte index, bool timeDependent = false)
{
    if (index >= TOUCH_COUNT)
        return;

    bool stateChanged =
        !relayState[index];

    bool modeChanged =
        relayTimeDependent[index] != timeDependent;

    relayTimeDependent[index] = timeDependent;

    if (stateChanged)
    {
        relayState[index] = true;
        updateOutputs();
    }

    if (stateChanged || modeChanged)
    {
        sys.relayNeedSave = true;
    }
}
void relayOff(byte index, bool timeDependent = false)
{
    if (index >= TOUCH_COUNT)
        return;

    bool stateChanged =
        relayState[index];

    bool modeChanged =
        relayTimeDependent[index] != timeDependent;

    relayTimeDependent[index] = timeDependent;

    if (stateChanged)
    {
        relayState[index] = false;
        updateOutputs();
    }

    if (stateChanged || modeChanged)
    {
        sys.relayNeedSave = true;
    }
}
//------------------------------------------------------

/*
void relayToggle(byte index)
{
    if (index >= TOUCH_COUNT)
        return;

    relayState[index] = !relayState[index];

    sys.relayNeedSave = true;

    updateOutputs();
}


void relayToggle(byte index, bool timeDependent = false)
{
    if (index >= TOUCH_COUNT)
        return;

    relayState[index] = !relayState[index];
    relayTimeDependent[index] = timeDependent;

    sys.relayNeedSave = true;

    updateOutputs();
}

//------------------------------------------------------
void manualRelayOn(byte index)
{
    if (index >= TOUCH_COUNT)
        return;

    cancelAutomationForRelay(index);
    relayOn(index, false);
}
*/
void relayToggle(byte index, bool timeDependent = false)
{
    if (index >= TOUCH_COUNT)
        return;

    relayState[index] = !relayState[index];

    relayTimeDependent[index] = timeDependent;

    sys.relayNeedSave = true;

    updateOutputs();
}
//------------------------------------------------------

void manualRelayOff(byte index)
{
    if (index >= TOUCH_COUNT)
        return;

    cancelAutomationForRelay(index);
    relayOff(index, false);
}

//------------------------------------------------------

void manualRelayToggle(byte index)
{
    if (index >= TOUCH_COUNT)
        return;

    cancelAutomationForRelay(index);
    relayToggle(index, false);
}


//------------------------------------------------------

bool relayGetState(byte index)
{
    if (index >= TOUCH_COUNT)
        return false;

    return relayState[index];
}

//------------------------------------------------------

void allRelayOff(bool timeDependent = false)
{
    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        relayState[i] = false;
        relayTimeDependent[i] = timeDependent;

    }

    sys.relayNeedSave = true;

    updateOutputs();
}

//------------------------------------------------------

void allRelayOn(bool timeDependent = false)
{
    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        relayState[i] = true;
                        relayTimeDependent[i] = timeDependent;

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
            if (touchFiresScenario[i])
            {
                fireScenarioLocal(touchScenarioId[i]);
            }
            else
            {
                // relayToggle(i);
                manualRelayToggle(i);
            }
			
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
        WiFi.softAP("SmartSwitch_SW000003", "12345678");

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

    //--------------------------------------------------
    // Peer Link (ANNOUNCE / SETRELAY / ACK / SCENE)
    //--------------------------------------------------

    handlePeerLinkPacket(packet, udp.remoteIP());
}

//=========================//15=============================
//                    WEB SERVER
//======================================================

void handleRoot()  // body webserver
{
  String html;

  html += "<!DOCTYPE html>";
  html += "<html>";
  html += "<head>"; 
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>"; 
  html += "<title>ESP8266 Test</title>"; 
  html += "<style>"; 
  html += "body{font-family:Arial;text-align:center;background:#f0f0f0;margin:0;padding:20px;}";
  html += ".btn{width:220px;height:60px;font-size:22px;border:none;border-radius:10px;color:white;margin:10px;cursor:pointer;}";
  html += ".on{background:#2ecc71;}";
  html += ".off{background:#e74c3c;}";
  html += "</style>";
  html += "</head>";
  //________ جهت رفرش صفحه در صورت تغییرات جاوا اسکریپت 
  html += "<script>";

  html += "function toggleRelay(ch){";
  html += "fetch('/toggle?ch='+ch).then(update);";
  html += "}";

    //برای سینک کردن تایم روی ماژول با مرورگر
  html += "function update(){";
  html += "let t=Math.floor(Date.now()/1000);";
  html += "fetch('/status?utc='+t)"; 
  html += ".then(r=>r.json())";
  html += ".then(function(d){";
  html += "for(let i=0;i<";
  html += String(TOUCH_COUNT);
  html += ";i++){";
  html += "let b=document.getElementsByClassName('btn')[i];";
  html += "if(d.relay[i]==1){";
  html += "b.className='btn on';";
  html += "b.innerHTML='Relay '+(i+1)+' ON';";
  html += "}";
  html += "else{";
  html += "b.className='btn off';";
  html += "b.innerHTML='Relay '+(i+1)+' OFF';";
  html += "}";
  html += "}";
  html += "});";
  html += "}";
  html += "setInterval(update,300);";
  html += "window.onload=update;";
  html += "</script>";
  //__________
    html += "<body>";
    html += "<h2>Smart KEY KP-elc Test</h2>";
    html += "<p><b>IP :</b> ";
    html += WiFi.localIP().toString();
    html += "</p>";
    html += "<p><b>RSSI :</b> ";
    html += String(WiFi.RSSI());
    html += " dBm</p>"; 
    html += "<p><b>Host :</b> ";  //  جهت نمایش نام دستگاه در mDNS
    html += hostName;
    html += ".local</p>"; 
    for(int i=0;i<TOUCH_COUNT;i++)
    {
      html += "<button class='btn "; 
      if(relayState[i])
        html += "on";
      else
        html += "off"; 
      html += "' onclick='toggleRelay(";
      html += String(i);
      html += ")'>"; 
      html += "Relay ";
      html += String(i+1); 
      if(relayState[i])
        html += " ON";
      else
        html += " OFF"; 
      html += "</button>"; 
    }

    html += "</body>";
    html += "</html>";

    server.send(200,"text/html",html);
}

//------------------------------------------------------

void handleStatus()
{
    // --------------------------------------------------
    // Fallback Time Source
    // فقط وقتی اجرا می‌شود که quality فعلاً SYNCED نیست —
    // یعنی NTP یا اصلاً sync نشده یا کهنه شده است.
    // اگر NTP تازه و SYNCED باشد، این بلاک اصلاً اجرا نمی‌شود
    // و اولویت NTP > Client به‌طور خودکار رعایت می‌شود.
    // --------------------------------------------------
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

    strcpy(config.deviceName, "SW000003");
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
            html += "<br><b>Group :</b> ";
            html += config.groupId;
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

        if (t.blinkerCount == 0)
        {
            server.send(
                400,
                "text/plain",
                "Invalid Blinker count"
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

void handleGetTimers() // <esp_IP>/getTimers
{
    String json = "[";

    for (uint8_t i = 0; i < MAX_TIMERS; i++)
    {
        Timer &t = timers[i];

        if (i > 0)
            json += ",";

        json += "{";
        json += "\"slot\":"     + String(i) + ",";
        json += "\"enabled\":"  + String(t.enabled ? 1 : 0) + ",";
        json += "\"type\":"     + String((uint8_t)t.type) + ",";
        json += "\"hour\":"     + String(t.hour) + ",";
        json += "\"minute\":"   + String(t.minute) + ",";
        json += "\"weekMask\":" + String(t.weekMask) + ",";
        json += "\"relay\":"    + String(t.relayIndex) + ",";
        json += "\"state\":"    + String(t.relayState ? 1 : 0) + ",";
        json += "\"duration\":" + String(t.durationSeconds);
        json += "}";
    }

    json += "]";

    server.send(200, "application/json", json);
}

//------------------------------------------------------

void handleSetRule()
{
    if (!server.hasArg("slot"))
    {
        server.send(400, "text/plain", "Missing slot");
        return;
    }

    int slot = server.arg("slot").toInt();

    if (slot < 0 || slot >= MAX_RULES)
    {
        server.send(400, "text/plain", "Invalid slot");
        return;
    }

    Rule &r = rules[slot];

    if (server.hasArg("enabled"))
        r.enabled = server.arg("enabled").toInt() != 0;

    if (server.hasArg("triggerType"))
        r.triggerType = (uint8_t)server.arg("triggerType").toInt();

    if (server.hasArg("triggerSource"))
        r.triggerSource = (uint8_t)server.arg("triggerSource").toInt();

    if (server.hasArg("triggerValue"))
        r.triggerValue = server.arg("triggerValue").toInt() != 0;

    if (server.hasArg("conditionType"))
        r.conditionType = (uint8_t)server.arg("conditionType").toInt();

    if (server.hasArg("conditionSource"))
        r.conditionSource = (uint8_t)server.arg("conditionSource").toInt();

    if (server.hasArg("conditionValue"))
        r.conditionValue = server.arg("conditionValue").toInt() != 0;

    if (server.hasArg("actionType"))
        r.actionType = (uint8_t)server.arg("actionType").toInt();

    if (server.hasArg("actionTarget"))
        r.actionTarget = (uint8_t)server.arg("actionTarget").toInt();

    if (server.hasArg("actionIsRemote"))
        r.actionIsRemote = server.arg("actionIsRemote").toInt() != 0;

    if (server.hasArg("actionPeerIndex"))
        r.actionPeerIndex = (uint8_t)server.arg("actionPeerIndex").toInt();

    if (server.hasArg("triggerScenarioId"))
        r.triggerScenarioId = (uint8_t)server.arg("triggerScenarioId").toInt();

    if (server.hasArg("actionDelaySeconds"))
    {
        uint32_t delay =
            (uint32_t)server.arg("actionDelaySeconds").toInt();

        if (delay != 0 &&
            (delay < MIN_RULE_DELAY_SECONDS ||
             delay > MAX_RULE_DELAY_SECONDS))
        {
            server.send(
                400,
                "text/plain",
                "Invalid actionDelaySeconds"
            );

            return;
        }

        r.actionDelaySeconds = delay;
    }


    saveRules();

    if (debug)
    {
        DBG_PRINT("Rule Saved: ");
        DBG_PRINTLN(slot);
    }

    server.send(200, "text/plain", "Rule Saved");
}

void handleGetRules() //<ESP-IP>//getRules
{
    String json = "[";

    for (uint8_t i = 0; i < MAX_RULES; i++)
    {
        if (i > 0)
            json += ",";

        Rule &r = rules[i];

        json += "{";

        json += "\"slot\":";
        json += i;

        json += ",\"enabled\":";
        json += r.enabled ? "true" : "false";

        json += ",\"triggerType\":";
        json += r.triggerType;

        json += ",\"triggerSource\":";
        json += r.triggerSource;

        json += ",\"triggerValue\":";
        json += r.triggerValue ? "true" : "false";

        json += ",\"lastTriggerState\":";
        json += r.lastTriggerState ? "true" : "false";

        json += ",\"conditionType\":";
        json += r.conditionType;

        json += ",\"conditionSource\":";
        json += r.conditionSource;

        json += ",\"conditionValue\":";
        json += r.conditionValue ? "true" : "false";

        json += ",\"actionType\":";
        json += r.actionType;

        json += ",\"actionTarget\":";
        json += r.actionTarget;


        json += ",\"actionDelaySeconds\":";
        json += r.actionDelaySeconds;


        json += "}";
    }

    json += "]";

    server.send(200, "application/json", json);
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
/*
  {
      if (outputsReady)
          return;

      unsigned long elapsed = millis() - bootRelayDelayStart;

      if (elapsed < BOOT_RELAY_MIN_DELAY_MS)
          return;

      bool catchupDone =
          (timeManager.quality != TIME_UNAVAILABLE) &&
          schedulerCatchupApplied;

      if (!catchupDone && elapsed < BOOT_RELAY_MAX_WAIT_MS)
          return;

      outputsReady = true;

      updateOutputs();

      if (debug)
      {
          if (catchupDone)
              DBG_PRINTLN("Boot Relay Restore: outputs enabled (after catch-

  up)");
          else
              DBG_PRINTLN("Boot Relay Restore: outputs enabled (timeout, no time 

  sync)");
      }
  }
*/

//------------------------------------------------------

void initStorage()
{
    LittleFS.begin();

    if (!loadConfig())
    {
        strcpy(config.deviceName, "SW000003");
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
    
      for (byte i = 0; i < TOUCH_COUNT; i++)
      {
          if (!relayRestoreOnBoot[i])
          {
              relayState[i] = false;   // بازیابی این رله غیرفعاله؛ صرف‌نظر از هر چیزی خاموش بمونه
              continue;
          }

           if (relayTimeDependent[i])
              relayState[i] = false;   // موقتاً در RAM؛ تا Catch-up تصمیم واقعی را بگیرد
      }

    
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
	server.on("/getRelayRestore", handleGetRelayRestore);
	server.on("/setRelayRestore", handleSetRelayRestore);
	server.on("/getPeerLinks", handleGetPeerLinks);
	server.on("/setPeerLink", handleSetPeerLink);
	server.on("/peerlinkTarget", handlePeerLinkEnsurePeer);
	server.on("/setTouchScenario", handleSetTouchScenario);
	server.on("/setScene", handleAddSceneAction);
	server.on("/fireScenario", handleFireScenario);
	server.on("/setPeerLinkSecret", handleSetPeerLinkSecret);
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
    server.on("/getTimers", handleGetTimers);
    server.on("/setRule", handleSetRule);
    server.on("/getRules", handleGetRules);



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

    loadPersistentData();

    initBootManager();


    initMCP();
    initWiFi();
    initTimeManager();

    loadRules();

    saveRules();
    initUDP();
    initWebServer();
    initPeerLink();
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
        taskBootRelayRestore();      // <-- برای برگشتن به حالت قبل از قطع برق و رفع مشکل pwm
                    taskTimeManager();

          //       taskTimeManager();
          //           taskScheduler();      // <--  مربوط بهScheduler این خط اضافه شد
          //             taskRuleManager(); // <--  مربوط به rule manager این خط اضافه شد

          //             processPendingRuleActions();   //  تاخیر در  رول ها است مربوط به 

          // taskWebServer(); 
          // taskUDP(); 
          // taskButton(); 
          // taskTouch(); 
          //==================================================
    // AUTOMATION GATE
    // Timer / Blinker / Rule / Pending Rule
    // فقط بعد از معتبر شدن زمان اجرا شوند
    //==================================================

    if (timeManager.quality == TIME_SYNCED)
    {
        taskScheduler();
        taskRuleManager();
        processPendingRuleActions();
    }
    // -----------------------------
    // MANUAL INPUTS — HIGHEST PRIORITY
    // -----------------------------
                    taskWebServer();
                    taskUDP(); 
                    taskPeerLink();
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
    // WiFi unavailable
    // --------------------------------------------------
    if (!sys.wifiConnected)
        return;

    // --------------------------------------------------
    // First NTP Sync - فقط درخواست را می‌فرستد
    // --------------------------------------------------
    if (!timeManager.ntpSynced &&
        timeManager.lastSyncMillis == 0)
    {
        syncTime();
    }

    // --------------------------------------------------
    // Periodic Resync - فقط درخواست را می‌فرستد
    // --------------------------------------------------
    if (millis() - timeManager.lastSyncMillis >=
        TIME_RESYNC_INTERVAL)
    {
        if (debug)
            DBG_PRINTLN("Time Periodic Resync");

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
        NTP_SERVERS[0],
        NTP_SERVERS[1],
        NTP_SERVERS[2]
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

    if (debug & timeManager.quality!= TIME_SYNCED)
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
        "UTC:%s |Qu:%s |Src:%s |ntpSyn:%s\n",// "[millis=%lu] SysClock UTC: %s | Q: %s | Source: %s | ntpSynced: %s\n",
        buffer, qualityStr, sourceStr,//millis(), buffer, qualityStr, sourceStr,
        timeManager.ntpSynced ? "true" : "false"
    );
}

void applyClientTimeSync(time_t clientUtc)
{
    if (timeManager.quality == TIME_SYNCED)
        return;   // NTP تازه است - اولویت با NTP، Client نادیده گرفته می‌شود

    if (clientUtc <= 1700000000)
        return;   // مقدار نامعتبر

    struct timeval tv = { clientUtc, 0 };
    settimeofday(&tv, nullptr);

    timeManager.ntpSynced    = true;
    timeManager.source       = SOURCE_CLIENT;
    timeManager.lastSyncTime = clientUtc;

    if (!debug)
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
            !(t.weekMask & (1 << nowWeekday)))
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
    static bool catchupApplied = false;

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

            if (timeManager.quality != TIME_SYNCED)
                return;
    //==================================================
    // CATCH-UP
    //==================================================

    if (!catchupApplied)
    {
        applyCatchupState();
        catchupApplied = true;
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
        if (t.weekMask & (1 << nowWeekday))
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
            if (t.weekMask & (1 << nowWeekday))
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
            if (t.type != TIMER_WEEKLY || (t.weekMask & (1 << todayWeekday)))
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
                (t.type != TIMER_WEEKLY || (t.weekMask & (1 << yestWeekday))))
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
        if (!relayRestoreOnBoot[relay])
            continue;   // این رله بعد از بوت همیشه خاموشه؛ کچ‌آپ روش اعمال نمی‌شه

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
                    (t.weekMask & (1 << nowWeekday));
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
                        (t.weekMask &
                         (1 << yesterdayWeekday));
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

        if (!relayRestoreOnBoot[t.relayIndex])
            continue;   // این رله بعد از بوت همیشه خاموشه؛ کچ‌آپ روش اعمال نمی‌شه

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

        if (!relayRestoreOnBoot[t.relayIndex])
            continue;   // این رله بعد از بوت همیشه خاموشه؛ کچ‌آپ روش اعمال نمی‌شه

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
                        (t.weekMask &
                         (1 << nowWeekday));
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
                            (t.weekMask &
                             (1 << yesterdayWeekday));
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

        uint32_t totalSeconds =
            (uint32_t)t.blinkerCount *
            cycleSeconds;
      if (cycleSeconds == 0 ||
            totalSeconds == 0)
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

        if ((uint32_t)elapsed >= totalSeconds)
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
            occurrenceStart +
            totalSeconds;

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

    if (f.size() == sizeof(timers))
        f.read((uint8_t *)timers, sizeof(timers));
    else
        memset(timers, 0, sizeof(timers));

    f.close();
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
//                  Rules
//======================================================

bool checkRuleTrigger(Rule &r)
{
    if (!r.enabled)
        return false;

    if (r.triggerType == RULE_TRIGGER_NONE)
        return false;
          /*
          //     if (r.triggerType == RULE_TRIGGER_RELAY_STATE)
          //     {
          //         if (r.triggerSource >= TOUCH_COUNT)
          //             return false;

          //         bool currentState = relayGetState(r.triggerSource);

          //         bool triggered = (currentState == r.triggerValue && r.lastTriggerState != currentState);
          //         r.lastTriggerState = currentState;

          // if (triggered && debug)
          // {
          //     DBG_PRINT("RULE TRIGGERED: ");
          //     DBG_PRINTLN(r.triggerSource);
          // }
          //         return triggered;
          //     }

          //     return false;
          // }
          */

    if (r.triggerType == RULE_TRIGGER_RELAY_STATE)
    {
        if (r.triggerSource >= TOUCH_COUNT)
            return false;

        bool currentState = relayGetState(r.triggerSource);							   
       // return (currentState == r.triggerValue);
        // لبه (Edge): فقط لحظه‌ای که وضعیت واقعاً عوض می‌شه true برگردون،
        // نه هر Tick که همچنان همون حالته — وگرنه برای Action از نوع
        // TOGGLE هر Tick دوباره اجرا می‌شه (و برای Remote یعنی سیل UDP).
        //bool triggered = (currentState == r.triggerValue) && (r.lastTriggerState != currentState);
        //r.lastTriggerState = currentState;

       // if (triggered && debug)
        //{
         //   DBG_PRINT("RULE TRIGGERED: ");
          //  DBG_PRINTLN(r.triggerSource);
        //}

        //return triggered;
        // توجه: این‌جا عمداً فقط سطح (Level) چک می‌شه، بدون دستکاری
        // r.lastTriggerState — چون تشخیص لبه (Edge) از قبل، درست،
        // یه‌لایه بالاتر توی executeRule() با همین فیلد انجام می‌شه.
        // نوشتن توی r.lastTriggerState این‌جا هم باعث می‌شد دو مکانیزم
        // همزمان یه فیلد مشترک رو بازنویسی کنن و با هم تداخل کنن.
        return (currentState == r.triggerValue);
						 
    }

    if (r.triggerType == RULE_TRIGGER_SCENARIO)
    {
        if (r.lastScenarioEvent == scenarioEventCounter)
            return false;   // این رویداد رو قبلاً دیدیم

        r.lastScenarioEvent = scenarioEventCounter;   // رویداد رو "دیده‌شده" علامت بزن

        return (r.triggerScenarioId == lastFiredScenarioId);
    }

    return false;
}

bool checkRuleCondition(Rule &r)
{
    if (r.conditionType == RULE_CONDITION_NONE)
        return true;

    if (r.conditionType == RULE_CONDITION_RELAY_STATE)
    {
        if (r.conditionSource >= TOUCH_COUNT)
            return false;

      bool result =
        relayGetState(r.conditionSource) == r.conditionValue;

  if (debug)
  {
      DBG_PRINT("RULE CONDITION: ");
      DBG_PRINTLN(result ? "TRUE" : "FALSE");
  }

        return result;


    }

    return false;
}

void executeRuleAction(Rule &r)
{
  
  if (debug)
  {
      DBG_PRINT("RULE ACTION: ");
      DBG_PRINTLN(r.actionTarget);
  }

    if (r.actionTarget >= TOUCH_COUNT)
        return;

    if (r.actionIsRemote)
    {
        bool desiredOn;

        switch (r.actionType)
        {
            case RULE_ACTION_RELAY_ON:  desiredOn = true;  break;
            case RULE_ACTION_RELAY_OFF: desiredOn = false; break;
            default:
                // TOGGLE روی یک Peer معنی مطمئنی نداره (وضعیت فعلیِ اون‌طرف
                // رو نمی‌دونیم)؛ Actionهای Remote فقط ON/OFF پشتیبانی می‌شن.
                if (debug)
                    DBG_PRINTLN("RULE ACTION: remote TOGGLE not supported, ignored");
                return;
        }

        sendCommandToPeer(r.actionPeerIndex, r.actionTarget, desiredOn);
        return;
    }
    switch (r.actionType)
    {
        case RULE_ACTION_RELAY_ON:
            relayOn(r.actionTarget);
            break;

        case RULE_ACTION_RELAY_OFF:
            relayOff(r.actionTarget);
            break;

        case RULE_ACTION_RELAY_TOGGLE:
            relayToggle(r.actionTarget);
            break;

        default:
            break;
    }
}



void executeRule(Rule &r, uint8_t ruleIndex)
{
    bool triggerState = checkRuleTrigger(r);

    // Trigger از ON به OFF یا OFF به ON تغییر کرده
    bool triggerChanged =
        (triggerState != r.lastTriggerState);

    // وضعیت Trigger را برای چرخه بعدی نگه می‌داریم
    r.lastTriggerState = triggerState;

    // فقط تغییر واقعی Trigger برای Rule مهم است
    if (!triggerChanged)
        return;

    if (debug)
    {
        DBG_PRINT("RULE TRIGGERED: ");
        DBG_PRINTLN(ruleIndex);
    }
	// اگر Trigger از بین رفته، Pending Action قبلی این Rule دیگر معتبر
    // نیست — ولی این فقط برای Triggerهای سطحی (RELAY_STATE) معنی داره.
    // SCENARIO یک Trigger لحظه‌ای (Pulse) است: یک Tick بعد از فعال شدن،
    // خودش به‌طور طبیعی false می‌شه، و نباید Actionِ همین الان
    // زمان‌بندی‌شده‌ش رو لغو کنه.
	if (!triggerState)
    {
        if (r.triggerType == RULE_TRIGGER_RELAY_STATE)
            cancelRuleAction(ruleIndex);
        return;
    }

    // Trigger فعال شده؛ حالا Condition بررسی شود
    if (!checkRuleCondition(r))
    {
        if (debug)
        {
            DBG_PRINT("RULE CONDITION: FALSE");
            DBG_PRINTLN("");
        }

        return;
    }

    if (debug)
    {
        DBG_PRINT("RULE CONDITION: TRUE");
        DBG_PRINTLN("");
    }

    // Action فوری
    if (r.actionDelaySeconds == 0)
    {
        executeRuleAction(r);
    }
    else
    {
        scheduleRuleAction(
            ruleIndex,
            r.actionType,
            r.actionTarget,
            r.actionIsRemote,
            r.actionPeerIndex,
            r.actionDelaySeconds
        );
    }
}



void taskRuleManager()
{
    for (uint8_t i = 0; i < MAX_RULES; i++)
    {
          Rule &r = rules[i];

        if (!r.enabled)
            continue;

        executeRule(r,i);
    }
}

void saveRules()
{
    File f = LittleFS.open(RULES_FILE, "w");

    if (!f)
    {
        if (debug)
            DBG_PRINTLN("Rules: save failed");

        return;
    }

    size_t written = f.write(
        (const uint8_t*)rules,
        sizeof(rules)
    );

    f.close();

    if (debug)
    {
        if (written == sizeof(rules))
            DBG_PRINTLN("Rules: saved");
        else
            DBG_PRINTLN("Rules: save incomplete");
    }
}

void loadRules()
{
    memset(rules, 0, sizeof(rules));

    if (!LittleFS.exists(RULES_FILE))
    {
        if (debug)
            DBG_PRINTLN("Rules: file not found");

        return;
    }

    File f = LittleFS.open(RULES_FILE, "r");

    if (!f)
    {
        if (debug)
            DBG_PRINTLN("Rules: open failed");

        return;
    }

    if (f.size() != sizeof(rules))
    {
        f.close();

        memset(rules, 0, sizeof(rules));

        if (debug)
            DBG_PRINTLN("Rules: invalid size");

        return;
    }

    size_t readSize = f.read(
        (uint8_t*)rules,
        sizeof(rules)
    );

    f.close();

    if (readSize != sizeof(rules))
    {
        memset(rules, 0, sizeof(rules));

        if (debug)
            DBG_PRINTLN("Rules: read failed");

        return;
    }

    if (debug)
        DBG_PRINTLN("Rules: loaded");
}

bool scheduleRuleAction(
    uint8_t ruleIndex,
    uint8_t actionType,
    uint8_t actionTarget,
    bool actionIsRemote,
    uint8_t actionPeerIndex,
    uint32_t delaySeconds
)
{
    // لغو Pending Action قبلی همین Rule
    for (uint8_t i = 0; i < MAX_PENDING_ACTIONS; i++)
    {
        if (pendingRuleActions[i].active &&
            pendingRuleActions[i].ruleIndex == ruleIndex)
        {
            pendingRuleActions[i].active = false;
        }
    }

    // پیدا کردن Slot آزاد
    for (uint8_t i = 0; i < MAX_PENDING_ACTIONS; i++)
    {
        if (!pendingRuleActions[i].active)
        {
            pendingRuleActions[i].active = true;

            pendingRuleActions[i].ruleIndex =
                ruleIndex;

            pendingRuleActions[i].actionType =
                actionType;

            pendingRuleActions[i].actionTarget =
                actionTarget;

            pendingRuleActions[i].actionIsRemote =
                actionIsRemote;

            pendingRuleActions[i].actionPeerIndex =
                actionPeerIndex;

            pendingRuleActions[i].executeAt =
                millis() +
                (delaySeconds * 1000UL);

            if (debug)
            {
                DBG_PRINT("Rule Action Scheduled: ");
                DBG_PRINT(ruleIndex);

                DBG_PRINT(" Target: ");
                DBG_PRINT(actionTarget);

                DBG_PRINT(" Delay: ");
                DBG_PRINT(delaySeconds);

                DBG_PRINTLN(" sec");
            }

            return true;
        }
    }

    if (debug)
        DBG_PRINTLN(
            "Rule Action Schedule Failed: No Slot"
        );

    return false;
}

void cancelRuleAction(uint8_t ruleIndex)
{
    for (uint8_t i = 0; i < MAX_PENDING_ACTIONS; i++)
    {
        if (pendingRuleActions[i].active &&
            pendingRuleActions[i].ruleIndex == ruleIndex)
        {
            pendingRuleActions[i].active = false;

            if (debug)
            {
                DBG_PRINT("Pending Rule Action Cancelled: ");
                DBG_PRINTLN(ruleIndex);
            }
        }
    }
}

void processPendingRuleActions()
{
    unsigned long nowMs = millis();

    for (uint8_t i = 0; i < MAX_PENDING_ACTIONS; i++)
    {
        PendingRuleAction &p = pendingRuleActions[i];

        if (!p.active)
            continue;

        if ((long)(nowMs - p.executeAt) >= 0)
        {
            p.active = false;

            Rule tempRule = {};   // صفر-مقداردهی کامل؛ قبلاً فیلدهای نخونده‌شده مقدار نامعلوم استک داشتن
            tempRule.actionType      = p.actionType;
            tempRule.actionTarget    = p.actionTarget;
            tempRule.actionIsRemote  = p.actionIsRemote;
            tempRule.actionPeerIndex = p.actionPeerIndex;
            executeRuleAction(tempRule);

            if (debug)
            {
                DBG_PRINT("Pending Rule Action Executed: ");
                DBG_PRINTLN(p.ruleIndex);
            }
        }
    }
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

    uint32_t totalSeconds =
        (uint32_t)t.blinkerCount * cycleSeconds;

    time_t elapsed = now - t.blinkStartTime;

    if (elapsed < 0)
        return false;

    if ((uint32_t)elapsed >= totalSeconds)
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

    uint32_t totalSeconds =
        (uint32_t)t.blinkerCount * cycleSeconds;

    time_t elapsed =
        now - t.blinkStartTime;

    if (elapsed < 0)
        return;

    // Blinker finished
    if ((uint32_t)elapsed >= totalSeconds)
    {
        relayOff(t.relayIndex);

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

    uint32_t totalSeconds =
        (uint32_t)t.blinkerCount * cycleSeconds;

    t.blinkStartTime =
    startTime;

    t.blinkerOccurrenceStart =
    startTime;

    t.blinkEndTime =
        startTime + totalSeconds;

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

