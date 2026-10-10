//======================================================
//   PEER LINK — ارتباط محلی بین دستگاه‌ها (بدون اینترنت، بدون Master)
//======================================================
// این تب، همونیه که فایل اصلی بهش اشاره می‌کنه («تعریف کامل توی تب
// PeerLink.ino»). همه‌ی توابعی که فایل اصلی از قبل صداشون می‌زنه یا
// به‌عنوان Route ثبتشون کرده (initPeerLink, taskPeerLink,
// handlePeerLinkPacket, handleGetPeerLinks, handleSetPeerLink,
// handlePeerLinkEnsurePeer, handleSetTouchScenario, handleAddSceneAction,
// handleFireScenario, handleSetPeerLinkSecret) همین‌جا تعریف می‌شن.
//
// معماری خلاصه:
//   - ANNOUNCE   : هر دستگاه هر ~۶۰ ثانیه (با Jitter) Broadcast می‌کنه
//                  «من اینجام». فقط IP دستگاه‌هایی که از قبل توی
//                  جدول Peer ثبت شدن (با peerlinkTarget) به‌روز می‌شه.
//   - CMD / ACK  : Unicast (نه Broadcast). فرستنده منتظر ACK می‌مونه؛
//                  اگه نیومد، چند بار Retry می‌کنه؛ همه‌ش غیر-بلاکینگ.
//   - SCENE      : Broadcast (چون منبعش بیرون از همه‌ی دستگاه‌هاست:
//                  اپ یا یه دستگاه دیگه که دکمه‌ی سناریو رو زده).
//
// نکته‌ی مهم درباره‌ی Dedup: چون Actionهای Remote فقط از نوع SET
// هستن (ON/OFF، نه TOGGLE)، و relayOn()/relayOff() خودشون already
// Idempotent هستن (فقط وقتی واقعاً تغییر کنه چیزی می‌نویسن)، دریافتِ
// یک CMD تکراری (مثلاً به‌خاطر Retry) کاملاً بی‌خطره و نیازی به جدول
// Dedup جداگانه روی گیرنده نیست.

//------------------------------------------------------
//                  تنظیمات قابل تغییر
//------------------------------------------------------
#define MAX_PEERS                 8
#define PEER_FILE                 "/peers.dat"
#define PEERLINK_SECRET_FILE      "/plsecret.dat"
#define TOUCH_SCENARIO_FILE       "/touchscn.dat"

#define PEER_OFFLINE_MS           180000UL   // 3 دقیقه بدون ANNOUNCE = آفلاین
#define ANNOUNCE_INTERVAL_MS      60000UL
#define ANNOUNCE_JITTER_MS        10000UL    // ± تصادفی، برای پخش‌شدن بین دستگاه‌ها

#define MAX_PENDING_CMD           8
#define ACK_TIMEOUT_DEFAULT_MS    350UL
#define CMD_MAX_RETRIES_DEFAULT   5

//------------------------------------------------------
//                  ساختارهای داده
//------------------------------------------------------
struct Peer
{
    bool     used;
    char     deviceId[13];     // MAC بدون ':'  مثل A1B2C3D4E5F6
    uint32_t ip;                // IPAddress به‌صورت عدد ساده (برای Serialize امن)
    uint32_t lastSeenMs;
};
Peer peers[MAX_PEERS];

enum PendingCmdState : uint8_t { PCMD_FREE = 0, PCMD_WAITING_ACK };

struct PendingCmd
{
    PendingCmdState state;
    uint16_t  commandId;
    uint8_t   peerIndex;
    uint8_t   targetRelay;
    bool      desiredState;
    uint32_t  lastSentAtMs;      // 0 = هنوز اصلاً فرستاده نشده
    uint8_t   retryCount;
    uint8_t   maxRetries;
    uint16_t  ackTimeoutMs;
};
PendingCmd pendingCmds[MAX_PENDING_CMD];

// ردیابی آخرین commandId دیده‌شده از هر منبع (سمت گیرنده‌ی CMD)
// چرا لازمه: idempotent بودن relayOn/Off فقط جلوی اعمال "همون فرمان
// تکراری" رو می‌گیره، ولی اگه UDP دو فرمان متفاوت رو بی‌ترتیب تحویل بده
// (مثلاً OFF زودتر از یه ON قدیمی‌تر که دیر رسیده برسه)، بدون این چک،
// فرمان قدیمی‌تر که دیرتر می‌رسه می‌تونه رو دست فرمان جدیدتر رو بگیره
// و رله رو توی حالت اشتباه نگه داره. این جدول این مورد رو رد می‌کنه.
#define MAX_CMD_SOURCES 8
struct CmdSource
{
    bool     used;
    char     deviceId[13];
    uint16_t lastCmdId;
};
CmdSource cmdSources[MAX_CMD_SOURCES];

bool peerLinkShouldApply(const String &srcId, uint16_t cmdId)
{
    for (int i = 0; i < MAX_CMD_SOURCES; i++)
    {
        if (cmdSources[i].used && srcId == cmdSources[i].deviceId)
        {
            int16_t diff = (int16_t)(cmdId - cmdSources[i].lastCmdId);

            if (diff <= 0)
                return false;   // قدیمی‌تر یا تکراریه؛ اعمال نکن (ولی بازم Ack می‌خوره)

            cmdSources[i].lastCmdId = cmdId;
            return true;
        }
    }

    // منبع جدید: یه اسلات آزاد بگیر و قبول کن
    for (int i = 0; i < MAX_CMD_SOURCES; i++)
    {
        if (!cmdSources[i].used)
        {
            cmdSources[i].used = true;
            strncpy(cmdSources[i].deviceId, srcId.c_str(), sizeof(cmdSources[i].deviceId) - 1);
            cmdSources[i].lastCmdId = cmdId;
            return true;
        }
    }

    return true;   // جدول پر؛ محافظه‌کارانه قبول کن (بهتر از رد نادرست)
}

// پیش‌اعلان دستی: Arduino IDE یه باگ شناخته‌شده داره (تولید خودکار
// Prototype برای توابعی که پارامترشون یه struct محلی همین Tab هست، گاهی
// قبل از تعریف خودِ struct گذاشته می‌شه). همین یه خط جلوشو می‌گیره.
void peerLinkSendCmd(PendingCmd &p);

uint16_t nextCommandId        = 1;
unsigned long nextAnnounceAt  = 0;
bool peerLinkTimeSyncedForResync = false;

char peerLinkSecret[32] = "CHANGE_ME_SECRET";   // از وب (setPeerLinkSecret) قابل تغییره

// توجه: scenarioEventCounter و lastFiredScenarioId این‌جا تعریف نشدن —
// توی فایل اصلی (14050713_V1.ino) تعریف شدن، کنار touchFiresScenario،
// چون checkRuleTrigger توی همون تب، قبل از این تب، بهشون نیاز داره.

//------------------------------------------------------
//                  شناسه‌ی یکتای این دستگاه
//------------------------------------------------------
String peerLinkDeviceUID()
{
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    return mac;
}

//------------------------------------------------------
//  امضای سبک پیام (Keyed FNV-1a روی متن + Secret)
//  این یک HMAC واقعی نیست؛ فقط جلوی فرمان ساختگیِ یک دستگاه/کامپیوتر
//  تصادفی روی همون LAN رو می‌گیره. برای Production جدی‌تر، بعداً می‌شه
//  با HMAC-SHA256 (از طریق BearSSL که از قبل در هسته‌ی ESP8266 هست)
//  جایگزینش کرد؛ فرمت پیام طوریه که این ارتقا بدون شکستن سازگاری
//  امکان‌پذیره (فقط طول/محتوای tag عوض می‌شه).
//------------------------------------------------------
String peerLinkSign(const String &body)
{
    uint32_t hash = 2166136261UL;
    String data = body + "|" + peerLinkSecret;

    for (size_t i = 0; i < data.length(); i++)
    {
        hash ^= (uint8_t)data[i];
        hash *= 16777619UL;
    }

    char hex[9];
    snprintf(hex, sizeof(hex), "%08lx", (unsigned long)hash);
    return String(hex);
}

bool peerLinkVerify(const String &body, const String &tag)
{
    return peerLinkSign(body) == tag;
}

//------------------------------------------------------
//                  کمکی: تقسیم رشته با '|'
//------------------------------------------------------
int peerLinkSplit(const String &s, String *out, int maxParts)
{
    int count = 0;
    int start = 0;

    while (count < maxParts)
    {
        int p = s.indexOf('|', start);

        if (p < 0)
        {
            out[count++] = s.substring(start);
            break;
        }

        out[count++] = s.substring(start, p);
        start = p + 1;
    }

    return count;
}

//------------------------------------------------------
//                  Peer table: جستجو/وضعیت
//------------------------------------------------------
int findPeerByDeviceId(const char *deviceId)
{
    for (int i = 0; i < MAX_PEERS; i++)
        if (peers[i].used && strcmp(peers[i].deviceId, deviceId) == 0)
            return i;

    return -1;
}

bool isPeerOnline(int idx)
{
    if (idx < 0 || idx >= MAX_PEERS || !peers[idx].used)
        return false;

    if (peers[idx].lastSeenMs == 0)
        return false;   // هنوز هیچ ANNOUNCE‌ای ازش نیومده

    return (millis() - peers[idx].lastSeenMs) < PEER_OFFLINE_MS;
}

//------------------------------------------------------
//                  ذخیره/بارگذاری (Peer / Secret / Touch-Scenario)
//------------------------------------------------------
void savePeers()
{
    File f = LittleFS.open(PEER_FILE, "w");
    if (!f) return;
    f.write((uint8_t *)peers, sizeof(peers));
    f.close();
}

void loadPeers()
{
    memset(peers, 0, sizeof(peers));

    if (!LittleFS.exists(PEER_FILE))
        return;

    File f = LittleFS.open(PEER_FILE, "r");
    if (!f) return;

    if (f.size() == sizeof(peers))
        f.read((uint8_t *)peers, sizeof(peers));

    f.close();
}

void savePeerLinkSecret()
{
    File f = LittleFS.open(PEERLINK_SECRET_FILE, "w");
    if (!f) return;
    f.write((uint8_t *)peerLinkSecret, sizeof(peerLinkSecret));
    f.close();
}

void loadPeerLinkSecret()
{
    if (!LittleFS.exists(PEERLINK_SECRET_FILE))
        return;

    File f = LittleFS.open(PEERLINK_SECRET_FILE, "r");
    if (!f) return;

    if (f.size() == sizeof(peerLinkSecret))
        f.read((uint8_t *)peerLinkSecret, sizeof(peerLinkSecret));

    f.close();
}

void saveTouchScenarios()
{
    File f = LittleFS.open(TOUCH_SCENARIO_FILE, "w");
    if (!f) return;
    f.write((uint8_t *)touchFiresScenario, sizeof(touchFiresScenario));
    f.write((uint8_t *)touchScenarioId, sizeof(touchScenarioId));
    f.close();
}

void loadTouchScenarios()
{
    if (!LittleFS.exists(TOUCH_SCENARIO_FILE))
        return;

    File f = LittleFS.open(TOUCH_SCENARIO_FILE, "r");
    if (!f) return;

    size_t expected = sizeof(touchFiresScenario) + sizeof(touchScenarioId);

    if (f.size() == expected)
    {
        f.read((uint8_t *)touchFiresScenario, sizeof(touchFiresScenario));
        f.read((uint8_t *)touchScenarioId, sizeof(touchScenarioId));
    }

    f.close();
}

//------------------------------------------------------
//                  ANNOUNCE
//------------------------------------------------------
void peerLinkSendAnnounce()
{
    String body = "ANNOUNCE|" + peerLinkDeviceUID();
    String tag  = peerLinkSign(body);

    udp.beginPacket(IPAddress(255, 255, 255, 255), UDP_PORT);
    udp.print(body + "|" + tag);
    udp.endPacket();
}

//------------------------------------------------------
//  وقتی یه Peer که قبلاً آفلاین بود دوباره ANNOUNCE می‌فرسته،
//  وضعیت فعلی هر Rule محلی‌ای که به اون Peer وصله رو دوباره
//  (idempotent، SET) براش می‌فرستیم تا چیزی که حین آفلاین‌بودنش
//  از دست داده رو جبران کنه.
//  محدودیت صادقانه: این فقط برای Actionهای ON/OFF کار می‌کنه،
//  نه TOGGLE (که اصلاً روی Remote پشتیبانی نمی‌شه)، و فقط یک‌طرفه‌ست؛
//  برای Mirror دوطرفه باید دو تا Rule جدا (یکی برای هر جهت) ساخت.
//------------------------------------------------------
bool sendCommandToPeer(uint8_t peerIndex, uint8_t relay, bool desiredOn);   // پیش‌اعلان برای استفاده‌ی زودتر

void resyncPeer(uint8_t peerIdx)
{
    for (uint8_t i = 0; i < MAX_RULES; i++)
    {
        Rule &r = rules[i];

        if (!r.enabled || !r.actionIsRemote)
            continue;

        if (r.actionPeerIndex != peerIdx)
            continue;

        if (r.triggerType != RULE_TRIGGER_RELAY_STATE)
            continue;

        if (r.triggerSource >= TOUCH_COUNT)
            continue;

        // Avoid treating a time-driven relay's temporary boot-OFF state
        // as its real trigger before time sync and scheduler catch-up.
        if (relayTimeDependent[r.triggerSource] &&
            !hasValidClock())
            continue;

        if (r.conditionType == RULE_CONDITION_RELAY_STATE &&
            r.conditionSource < TOUCH_COUNT &&
            relayTimeDependent[r.conditionSource] &&
            !hasValidClock())
            continue;

        if (r.actionType != RULE_ACTION_RELAY_ON && r.actionType != RULE_ACTION_RELAY_OFF)
            continue;   // Resync فقط برای SET معنی داره

        if (relayGetState(r.triggerSource) != r.triggerValue)
            continue;   // الان شرط Trigger برقرار نیست، چیزی برای Resync نیست

        if (!checkRuleCondition(r))
            continue;

        sendCommandToPeer(r.actionPeerIndex, r.actionTarget, r.actionType == RULE_ACTION_RELAY_ON);

        if (debug)
        {
            DBG_PRINT("PeerLink: resync rule ");
            DBG_PRINT(i);
            DBG_PRINT(" -> peer ");
            DBG_PRINTLN(peerIdx);
        }
    }
}

void handleAnnouncePacket(const String &pkt, IPAddress remoteIP)
{
    String parts[3];
    int n = peerLinkSplit(pkt, parts, 3);
    if (n != 3) return;

    String body = parts[0] + "|" + parts[1];
    String tag  = parts[2];

    if (!peerLinkVerify(body, tag))
        return;

    String deviceId = parts[1];

    if (deviceId == peerLinkDeviceUID())
        return;   // Broadcast خودمون به خودمون هم می‌رسه؛ نادیده بگیر

    int idx = findPeerByDeviceId(deviceId.c_str());

    if (idx < 0)
        return;   // این deviceId توی جدول Peer ما ثبت نشده (فقط با peerlinkTarget اضافه می‌شه)

    bool wasOffline = !isPeerOnline(idx);

    peers[idx].ip = (uint32_t)remoteIP;
    peers[idx].lastSeenMs = millis();

    if (wasOffline)
        resyncPeer(idx);
}

//------------------------------------------------------
//                  CMD / ACK
//------------------------------------------------------
void peerLinkSendCmd(PendingCmd &p)
{
    String body = "CMD|" + peerLinkDeviceUID() + "|" + String(p.commandId) + "|" +
                  String(p.targetRelay) + "|" + (p.desiredState ? "SET_ON" : "SET_OFF");
    String tag = peerLinkSign(body);

    udp.beginPacket(IPAddress(peers[p.peerIndex].ip), UDP_PORT);
    udp.print(body + "|" + tag);
    udp.endPacket();

    p.lastSentAtMs = millis();
}

bool sendCommandToPeer(uint8_t peerIndex, uint8_t relay, bool desiredOn)
{
    if (peerIndex >= MAX_PEERS || !peers[peerIndex].used)
        return false;

    if (relay >= TOUCH_COUNT)
        return false;

    // اگه برای همین Peer+رله یه فرمان Pending هست، جاش رو با فرمان جدید
    // عوض کن (آخرین فرمان برنده‌ست، نه صف‌شدن فرمان‌های قدیمی‌تر پشت سرهم)
    int slot = -1;

    for (int i = 0; i < MAX_PENDING_CMD; i++)
    {
        if (pendingCmds[i].state == PCMD_WAITING_ACK &&
            pendingCmds[i].peerIndex == peerIndex &&
            pendingCmds[i].targetRelay == relay)
        {
            slot = i;
            break;
        }
    }

    if (slot < 0)
    {
        for (int i = 0; i < MAX_PENDING_CMD; i++)
        {
            if (pendingCmds[i].state == PCMD_FREE)
            {
                slot = i;
                break;
            }
        }
    }

    if (slot < 0)
    {
        if (debug)
            DBG_PRINTLN("PeerLink: pending queue full");

        return false;
    }

    uint16_t cmdId = nextCommandId++;
    if (nextCommandId == 0)
        nextCommandId = 1;   // صفر رو به‌عنوان Id معتبر استفاده نکن

    pendingCmds[slot].state        = PCMD_WAITING_ACK;
    pendingCmds[slot].commandId    = cmdId;
    pendingCmds[slot].peerIndex    = peerIndex;
    pendingCmds[slot].targetRelay  = relay;
    pendingCmds[slot].desiredState = desiredOn;
    pendingCmds[slot].lastSentAtMs = 0;   // یعنی taskPeerLink همین Tick اول بار بفرسته
    pendingCmds[slot].retryCount   = 0;
    pendingCmds[slot].maxRetries   = CMD_MAX_RETRIES_DEFAULT;
    pendingCmds[slot].ackTimeoutMs = ACK_TIMEOUT_DEFAULT_MS;

    return true;
}

void handleCmdPacket(const String &pkt, IPAddress remoteIP)
{
    String parts[6];
    int n = peerLinkSplit(pkt, parts, 6);
    if (n != 6) return;

    String body = parts[0] + "|" + parts[1] + "|" + parts[2] + "|" + parts[3] + "|" + parts[4];
    String tag  = parts[5];

    if (!peerLinkVerify(body, tag))
        return;

    String   srcId  = parts[1];
    uint16_t cmdId  = (uint16_t)parts[2].toInt();
    uint8_t  relay  = (uint8_t)parts[3].toInt();
    bool     wantOn = (parts[4] == "SET_ON");

    if (relay < TOUCH_COUNT && peerLinkShouldApply(srcId, cmdId))
    {
        // A relay commanded by another device is a client output; do not
        // persist its runtime ON/OFF state for restoration after reboot.
        setRelayRestoreOnBoot(relay, false, false);
        relaySetTransientState(relay, wantOn);
    }

    // همیشه ACK بفرست — چه اعمال شد چه (چون قدیمی/تکراری/بی‌ترتیب بود)
    // رد شد — فرستنده باید بدونه Retry دیگه لازم نیست.
    String ackBody = "ACK|" + peerLinkDeviceUID() + "|" + String(cmdId) + "|OK";
    String ackTag  = peerLinkSign(ackBody);

    udp.beginPacket(remoteIP, UDP_PORT);
    udp.print(ackBody + "|" + ackTag);
    udp.endPacket();
}

void handleAckPacket(const String &pkt)
{
    String parts[5];
    int n = peerLinkSplit(pkt, parts, 5);
    if (n != 5) return;

    String body = parts[0] + "|" + parts[1] + "|" + parts[2] + "|" + parts[3];
    String tag  = parts[4];

    if (!peerLinkVerify(body, tag))
        return;

    uint16_t cmdId = (uint16_t)parts[2].toInt();

    for (int i = 0; i < MAX_PENDING_CMD; i++)
    {
        if (pendingCmds[i].state == PCMD_WAITING_ACK &&
            pendingCmds[i].commandId == cmdId)
        {
            pendingCmds[i].state = PCMD_FREE;
            break;
        }
    }
}

//------------------------------------------------------
//                  SCENE / Scenario
//------------------------------------------------------
void fireScenarioLocal(uint8_t sceneId)
{
    lastFiredScenarioId = sceneId;
    scenarioEventCounter++;

    if (scenarioEventCounter == 0)
        scenarioEventCounter = 1;   // از سرریز به صفر رد شو (۰ یعنی «هنوز هیچ رویدادی نبوده»)

    if (debug)
    {
        DBG_PRINT("PeerLink: scenario fired: ");
        DBG_PRINTLN(sceneId);
    }

    // اجرای خودِ Ruleهای این سناریو با چرخه‌ی معمول taskRuleManager انجام
    // می‌شه (checkRuleTrigger اون‌جا scenarioEventCounter رو چک می‌کنه)؛
    // این‌جا فقط رویداد رو "اعلام" می‌کنیم.
}

void peerLinkBroadcastScene(uint8_t sceneId)
{
    String body = "SCENE|" + String(sceneId);
    String tag  = peerLinkSign(body);

    udp.beginPacket(IPAddress(255, 255, 255, 255), UDP_PORT);
    udp.print(body + "|" + tag);
    udp.endPacket();
}

void handleScenePacket(const String &pkt)
{
    String parts[3];
    int n = peerLinkSplit(pkt, parts, 3);
    if (n != 3) return;

    String body = parts[0] + "|" + parts[1];
    String tag  = parts[2];

    if (!peerLinkVerify(body, tag))
        return;

    fireScenarioLocal((uint8_t)parts[1].toInt());
}

//------------------------------------------------------
//  ورودی اصلی - از handleUDP() صدا زده می‌شه
//------------------------------------------------------
void handlePeerLinkPacket(const char *packet, IPAddress remoteIP)
{
    String pkt(packet);

    int p1 = pkt.indexOf('|');
    if (p1 < 0) return;

    String type = pkt.substring(0, p1);

    if (type == "ANNOUNCE") { handleAnnouncePacket(pkt, remoteIP); return; }
    if (type == "CMD")      { handleCmdPacket(pkt, remoteIP);      return; }
    if (type == "ACK")      { handleAckPacket(pkt);                return; }
    if (type == "SCENE")    { handleScenePacket(pkt);              return; }
}

//------------------------------------------------------
//  Init  (از startSystem() صدا زده می‌شه)
//------------------------------------------------------
void initPeerLink()
{
    loadPeerLinkSecret();
    loadPeers();
    loadTouchScenarios();

    randomSeed(ESP.getChipId() ^ micros());

    peerLinkTimeSyncedForResync = false;
    nextAnnounceAt = millis() + (unsigned long)random(0, (long)ANNOUNCE_JITTER_MS);
}

//------------------------------------------------------
//  Task  (از runSystem() صدا زده می‌شه؛ کاملاً غیر-بلاکینگ)
//------------------------------------------------------
void taskPeerLink()
{
    unsigned long now = millis();

    // If a peer announced while this device lacked valid time, resync again
    // after scheduler catch-up makes time-driven source states authoritative.
    bool clockValid = hasValidClock();
    if (clockValid && !peerLinkTimeSyncedForResync)
    {
        for (uint8_t i = 0; i < MAX_PEERS; i++)
        {
            if (isPeerOnline(i))
                resyncPeer(i);
        }
    }
    peerLinkTimeSyncedForResync = clockValid;

    // ۱) ANNOUNCE دوره‌ای
    if ((long)(now - nextAnnounceAt) >= 0)
    {
        peerLinkSendAnnounce();

        long jitter = random(-(long)ANNOUNCE_JITTER_MS, (long)ANNOUNCE_JITTER_MS);
        nextAnnounceAt = now + ANNOUNCE_INTERVAL_MS + jitter;
    }

    // ۲) صف فرمان‌های Pending: اولین ارسال / Retry / قطع نهایی
    for (int i = 0; i < MAX_PENDING_CMD; i++)
    {
        PendingCmd &p = pendingCmds[i];

        if (p.state != PCMD_WAITING_ACK)
            continue;

        if (p.lastSentAtMs == 0)
        {
            peerLinkSendCmd(p);
            continue;
        }

        if (now - p.lastSentAtMs < p.ackTimeoutMs)
            continue;

        if (p.retryCount >= p.maxRetries)
        {
            if (debug)
            {
                DBG_PRINT("PeerLink: CMD ");
                DBG_PRINT(p.commandId);
                DBG_PRINTLN(" FAILED (no ACK)");
            }

            p.state = PCMD_FREE;
            continue;
        }

        p.retryCount++;
        peerLinkSendCmd(p);
    }
}

//------------------------------------------------------
//  وب: مدیریت جدول Peer
//  بدون deviceId  → کل جدول رو برمی‌گردونه (برای نمایش)
//  با deviceId    → اگه نبود اضافه می‌کنه، اندیسش رو برمی‌گردونه
//------------------------------------------------------
void handlePeerLinkEnsurePeer()
{
    if (!server.hasArg("deviceId"))
    {
        String json;
        json.reserve(220);
        json += '[';

        for (int i = 0; i < MAX_PEERS; i++)
        {
            if (i) json += ',';

            json += "{\"index\":";       json += i;
            json += ",\"used\":";        json += (peers[i].used ? 1 : 0);
            json += ",\"deviceId\":\""; json += (peers[i].used ? peers[i].deviceId : "");
            json += "\",\"ip\":\"";      json += (peers[i].used ? IPAddress(peers[i].ip).toString() : String(""));
            json += "\",\"online\":";    json += (isPeerOnline(i) ? 1 : 0);
            json += '}';
        }

        json += ']';

        server.send(200, "application/json", json);
        return;
    }

    String devId = server.arg("deviceId");
    devId.toUpperCase();

    if (devId.length() == 0 || devId.length() > 12)
    {
        server.send(400, "text/plain", "Invalid deviceId");
        return;
    }

    int idx = findPeerByDeviceId(devId.c_str());

    if (idx < 0)
    {
        for (int i = 0; i < MAX_PEERS; i++)
        {
            if (!peers[i].used)
            {
                idx = i;
                break;
            }
        }

        if (idx < 0)
        {
            server.send(507, "text/plain", "No free peer slot");
            return;
        }

        memset(&peers[idx], 0, sizeof(Peer));
        peers[idx].used = true;
        strncpy(peers[idx].deviceId, devId.c_str(), sizeof(peers[idx].deviceId) - 1);

        savePeers();
    }

    server.send(200, "text/plain", String(idx));
}

//------------------------------------------------------
//  وب: کلید مشترک امضا
//------------------------------------------------------
void handleSetPeerLinkSecret()
{
    if (!server.hasArg("secret"))
    {
        server.send(400, "text/plain", "Missing secret");
        return;
    }

    String s = server.arg("secret");

    if (s.length() == 0 || s.length() >= sizeof(peerLinkSecret))
    {
        server.send(400, "text/plain", "Invalid secret length");
        return;
    }

    memset(peerLinkSecret, 0, sizeof(peerLinkSecret));
    strncpy(peerLinkSecret, s.c_str(), sizeof(peerLinkSecret) - 1);

    savePeerLinkSecret();

    server.send(200, "text/plain", "OK");
}

//------------------------------------------------------
//  وب: تاچ → سناریو
//------------------------------------------------------
void handleSetTouchScenario()
{
    if (!server.hasArg("touch"))
    {
        server.send(400, "text/plain", "Missing touch");
        return;
    }

    int t = server.arg("touch").toInt();

    if (t < 0 || t >= TOUCH_COUNT)
    {
        server.send(400, "text/plain", "Invalid touch");
        return;
    }

    if (server.hasArg("enabled"))
        touchFiresScenario[t] = server.arg("enabled").toInt() != 0;

    if (server.hasArg("scenario"))
        touchScenarioId[t] = (uint8_t)server.arg("scenario").toInt();

    saveTouchScenarios();

    server.send(200, "text/plain", "OK");
}

//------------------------------------------------------
//  وب: فرمان مستقیم اجرای سناریو (از صفحه‌ی خود دستگاه یا هر اپی
//  که روی همون LAN باشه) — هم محلی اجرا می‌شه هم Broadcast می‌شه
//  تا بقیه‌ی دستگاه‌ها هم باخبر بشن.
//------------------------------------------------------
void handleFireScenario()
{
    if (!server.hasArg("id"))
    {
        server.send(400, "text/plain", "Missing id");
        return;
    }

    uint8_t id = (uint8_t)server.arg("id").toInt();

    fireScenarioLocal(id);
    peerLinkBroadcastScene(id);

    server.send(200, "text/plain", "OK");
}

//------------------------------------------------------
//  وب: ساخت/ویرایش یک Rule از نوع «کلید تبدیل / صلیبی»
//  (Trigger = رله محلی، Action = SET روی رله‌ی یک Peer)
//  ذخیره‌سازی همون آرایه‌ی rules[] فعلیه؛ این فقط یه Endpoint
//  ساده‌تر برای همین کار خاصه (معادل handleSetRule با فیلدهای کمتر).
//------------------------------------------------------
void handleSetPeerLink()
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
    memset(&r, 0, sizeof(r));

    r.enabled       = server.hasArg("enabled") ? (server.arg("enabled").toInt() != 0) : true;

    r.triggerType   = RULE_TRIGGER_RELAY_STATE;
    r.triggerSource = server.hasArg("localRelay") ? (uint8_t)server.arg("localRelay").toInt() : 0;
    r.triggerValue  = server.hasArg("triggerValue") ? (server.arg("triggerValue").toInt() != 0) : true;

    r.conditionType = RULE_CONDITION_NONE;

    r.actionIsRemote  = true;
    r.actionPeerIndex = server.hasArg("peerIndex") ? (uint8_t)server.arg("peerIndex").toInt() : 0;
    r.actionTarget    = server.hasArg("remoteRelay") ? (uint8_t)server.arg("remoteRelay").toInt() : 0;
    r.actionType      = (server.hasArg("state") && server.arg("state").toInt() == 0)
                         ? RULE_ACTION_RELAY_OFF : RULE_ACTION_RELAY_ON;

    r.actionDelaySeconds = 0;

    if (r.triggerSource >= TOUCH_COUNT || r.actionTarget >= TOUCH_COUNT ||
        r.actionPeerIndex >= MAX_PEERS || !peers[r.actionPeerIndex].used)
    {
        r.enabled = false;   // پیکربندی نامعتبر؛ ذخیره می‌شه ولی غیرفعال
    }

    saveRules();

    server.send(200, "text/plain", "OK");
}

//------------------------------------------------------
//  وب: لیست Ruleهایی که Action‌شون Remote هست (برای نمایش در پنل)
//------------------------------------------------------
void handleGetPeerLinks()
{
    String json;
    json.reserve(400);
    json += '[';

    bool first = true;

    for (int i = 0; i < MAX_RULES; i++)
    {
        Rule &r = rules[i];

        if (!r.actionIsRemote)
            continue;

        if (!first) json += ',';
        first = false;

        json += "{\"slot\":";         json += i;
        json += ",\"enabled\":";      json += (r.enabled ? 1 : 0);
        json += ",\"localRelay\":";   json += (int)r.triggerSource;
        json += ",\"triggerValue\":"; json += (r.triggerValue ? 1 : 0);
        json += ",\"peerIndex\":";    json += (int)r.actionPeerIndex;
        json += ",\"remoteRelay\":";  json += (int)r.actionTarget;
        json += ",\"state\":";        json += (r.actionType == RULE_ACTION_RELAY_ON ? 1 : 0);
        json += '}';
    }

    json += ']';

    server.send(200, "application/json", json);
}

//------------------------------------------------------
//  وب: افزودن/ویرایش یک Rule از نوع سناریو (محلی یا Remote)
//  همون rules[] با triggerType = RULE_TRIGGER_SCENARIO
//------------------------------------------------------
void handleAddSceneAction()
{
    if (!server.hasArg("slot") || !server.hasArg("scene"))
    {
        server.send(400, "text/plain", "Missing slot/scene");
        return;
    }

    int slot = server.arg("slot").toInt();

    if (slot < 0 || slot >= MAX_RULES)
    {
        server.send(400, "text/plain", "Invalid slot");
        return;
    }

    Rule &r = rules[slot];
    memset(&r, 0, sizeof(r));

    r.enabled          = server.hasArg("enabled") ? (server.arg("enabled").toInt() != 0) : true;
    r.triggerType       = RULE_TRIGGER_SCENARIO;
    r.triggerScenarioId = (uint8_t)server.arg("scene").toInt();
    r.lastScenarioEvent = scenarioEventCounter;   // به رویدادهای گذشته واکنش نشون نده

    r.conditionType = RULE_CONDITION_NONE;

    bool remote = server.hasArg("remote") && (server.arg("remote").toInt() != 0);
    r.actionIsRemote = remote;

    r.actionTarget = server.hasArg("relay") ? (uint8_t)server.arg("relay").toInt() : 0;

    if (remote)
        r.actionPeerIndex = server.hasArg("peerIndex") ? (uint8_t)server.arg("peerIndex").toInt() : 0;

    r.actionType = (server.hasArg("state") && server.arg("state").toInt() == 0)
                   ? RULE_ACTION_RELAY_OFF : RULE_ACTION_RELAY_ON;

    r.actionDelaySeconds = server.hasArg("delay") ? (uint32_t)server.arg("delay").toInt() : 0;

    if (r.actionTarget >= TOUCH_COUNT ||
        (remote && (r.actionPeerIndex >= MAX_PEERS || !peers[r.actionPeerIndex].used)))
    {
        r.enabled = false;
    }

    saveRules();

    server.send(200, "text/plain", "OK");
}
