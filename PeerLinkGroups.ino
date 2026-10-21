//======================================================
//   PEER LINK GROUPS — گروه، نقش و نسخه‌دار کردن وضعیت رله‌ها
//======================================================
// هر کلید (= یک رله) می‌تواند عضو چند «گروه» باشد. هر گروه فقط یک شماره (uint32) است.
//
//   نقش «فرمان‌ده» (COMMANDER): تغییر رله‌ی این کلید برای اعضای همان گروه فرستاده می‌شود،
//                               و تغییر گروه را هم اعمال می‌کند (فرمان‌ده‌های یک گروه هم‌گام‌اند).
//   نقش «شنونده»   (LISTENER) : فقط تغییر گروه را اعمال می‌کند؛ تغییر محلی‌اش چیزی نمی‌فرستد.
//
// قانون: هر کلید حداکثر یک عضویت «فرمان‌ده» دارد، ولی هر تعداد عضویت «شنونده».
// یک فرمان‌ده می‌تواند در گروه‌های دیگر شنونده باشد؛ آن‌وقت هر تغییری که از آن گروه‌ها
// بگیرد، به اعضای گروه فرمان‌دهی خودش هم می‌رسد (مثلاً «همه‌ی روشنایی‌ها» ← کلیدهای راهرو).
//
// نسخه‌دار کردن (ساعت منطقی Lamport + شناسه‌ی دستگاه):
//   - هر تغییر محلیِ یک کلید فرمان‌ده برچسب (ts, origin) می‌گیرد.
//   - گیرنده فقط وقتی اعمال می‌کند که برچسب از برچسب فعلی رله‌اش «جدیدتر» باشد.
//   - فرمان قدیمی/تکراری/Echo خودبه‌خود رد می‌شود؛ بوت هم‌زمان و کلیدزدن هم‌زمان هم
//     روی یک نتیجه‌ی یکسان همگرا می‌شوند (برنده: ts بزرگ‌تر، بعد origin بزرگ‌تر).
//   - برچسب هر رله روی فلش (relayver.dat) ذخیره می‌شود.
//
// پیام شبکه:  GUPD|<دستگاه>|<cmdId>|<گروه>|<0/1>|<ts>|<origin>|<tag>   (Unicast، با ACK/Retry)
// اعضای یک گروه از قبل ثبت نمی‌شوند؛ تغییر برای همه‌ی Peerهای آنلاین فرستاده می‌شود و
// دستگاهی که عضو آن گروه نیست فقط ACK می‌دهد.

#define GROUPS_FILE              "/groups.dat"
#define GROUP_VER_FILE           "/relayver.dat"
#define GROUPS_MAGIC             0x31505247UL      // "GRP1"
#define GROUP_VER_MAGIC          0x31524556UL      // "VER1"

#define GROUP_MAX_MEMBERSHIPS    32      // به ازای هر کلید (فقط محدودیت حافظه؛ قابل افزایش)
#define GROUP_MAX_PENDING        64
#define GROUP_ACK_TIMEOUT_MS     350UL
#define GROUP_MAX_RETRIES        5

#define GROUP_ROLE_LISTENER      0
#define GROUP_ROLE_COMMANDER     1

struct GroupMembership
{
    uint32_t group;
    uint8_t  role;
};

struct RelayVersion
{
    uint32_t ts;
    uint32_t origin;
};

struct GroupPending
{
    bool     used;
    uint16_t commandId;
    uint8_t  peerIndex;
    bool     state;
    uint32_t group;
    uint32_t ts;
    uint32_t origin;
    uint32_t lastSentAtMs;      // 0 = هنوز فرستاده نشده
    uint8_t  retryCount;
};

GroupMembership groupMembers[TOUCH_COUNT][GROUP_MAX_MEMBERSHIPS];
uint8_t         groupMemberCount[TOUCH_COUNT];
RelayVersion    relayVer[TOUCH_COUNT];
GroupPending    groupPending[GROUP_MAX_PENDING];

uint32_t groupLamport        = 0;
bool     groupApplyingRemote = false;   // true = تغییر رله از Peer/کلید هم‌گروه آمده؛ نسخه‌ی جدید نمی‌گیرد

//------------------------------------------------------
//                  کمکی‌ها
//------------------------------------------------------
uint32_t groupNodeId()
{
    static uint32_t id = 0;

    if (id == 0)
    {
        String uid = peerLinkDeviceUID();      // ۱۲ رقم هگز

        if (uid.length() >= 12 && uid != "000000000000")
        {
            id = (uint32_t)strtoul(uid.substring(4).c_str(), nullptr, 16);   // ۴ بایت آخر MAC

            if (id == 0)
                id = 1;
        }
        else
        {
            return 1;   // هنوز MAC معتبر نیست؛ کش نکن
        }
    }

    return id;
}

// برچسب (ts, origin) از برچسب فعلی (curTs, curOrigin) جدیدتر است؟
bool groupVerNewer(uint32_t ts, uint32_t origin, uint32_t curTs, uint32_t curOrigin)
{
    if (ts != curTs)
        return ts > curTs;

    return origin > curOrigin;
}

int groupFindMember(uint8_t relay, uint32_t group)
{
    if (relay >= TOUCH_COUNT)
        return -1;

    for (uint8_t i = 0; i < groupMemberCount[relay]; i++)
        if (groupMembers[relay][i].group == group)
            return i;

    return -1;
}

uint8_t groupCommanderCount(uint8_t relay)
{
    uint8_t c = 0;

    if (relay >= TOUCH_COUNT)
        return 0;

    for (uint8_t i = 0; i < groupMemberCount[relay]; i++)
        if (groupMembers[relay][i].role == GROUP_ROLE_COMMANDER)
            c++;

    return c;
}

//------------------------------------------------------
//                  ذخیره / بارگذاری
//------------------------------------------------------
void groupSaveMembers()
{
    File f = LittleFS.open(GROUPS_FILE, "w");
    if (!f) return;

    uint32_t magic = GROUPS_MAGIC;
    f.write((uint8_t *)&magic, sizeof(magic));

    for (uint8_t r = 0; r < TOUCH_COUNT; r++)
    {
        uint8_t c = groupMemberCount[r];
        f.write(&c, 1);

        for (uint8_t i = 0; i < c; i++)
        {
            f.write((uint8_t *)&groupMembers[r][i].group, sizeof(uint32_t));
            f.write(&groupMembers[r][i].role, 1);
        }
    }

    f.close();
}

void groupLoadMembers()
{
    memset(groupMembers, 0, sizeof(groupMembers));
    memset(groupMemberCount, 0, sizeof(groupMemberCount));

    if (!LittleFS.exists(GROUPS_FILE))
        return;

    File f = LittleFS.open(GROUPS_FILE, "r");
    if (!f) return;

    uint32_t magic = 0;

    if (f.read((uint8_t *)&magic, sizeof(magic)) != sizeof(magic) || magic != GROUPS_MAGIC)
    {
        f.close();
        return;
    }

    for (uint8_t r = 0; r < TOUCH_COUNT; r++)
    {
        uint8_t c = 0;

        if (f.read(&c, 1) != 1 || c > GROUP_MAX_MEMBERSHIPS)
        {
            // فایل خراب: همه چیز را پاک کن
            memset(groupMembers, 0, sizeof(groupMembers));
            memset(groupMemberCount, 0, sizeof(groupMemberCount));
            f.close();
            return;
        }

        for (uint8_t i = 0; i < c; i++)
        {
            uint32_t g = 0;
            uint8_t  role = 0;

            if (f.read((uint8_t *)&g, sizeof(g)) != sizeof(g) || f.read(&role, 1) != 1 || role > GROUP_ROLE_COMMANDER)
            {
                memset(groupMembers, 0, sizeof(groupMembers));
                memset(groupMemberCount, 0, sizeof(groupMemberCount));
                f.close();
                return;
            }

            groupMembers[r][i].group = g;
            groupMembers[r][i].role  = role;
        }

        groupMemberCount[r] = c;
    }

    f.close();
}

// از taskRelaySave() (فایل اصلی) هم‌زمان با ذخیره‌ی وضعیت رله‌ها صدا زده می‌شود
void groupSaveVersions()
{
    File f = LittleFS.open(GROUP_VER_FILE, "w");
    if (!f) return;

    uint32_t magic = GROUP_VER_MAGIC;
    f.write((uint8_t *)&magic, sizeof(magic));
    f.write((uint8_t *)&groupLamport, sizeof(groupLamport));
    f.write((uint8_t *)relayVer, sizeof(relayVer));
    f.close();
}

void groupLoadVersions()
{
    memset(relayVer, 0, sizeof(relayVer));
    groupLamport = 0;

    if (!LittleFS.exists(GROUP_VER_FILE))
        return;

    File f = LittleFS.open(GROUP_VER_FILE, "r");
    if (!f) return;

    if (f.size() == sizeof(uint32_t) + sizeof(groupLamport) + sizeof(relayVer))
    {
        uint32_t magic = 0;
        f.read((uint8_t *)&magic, sizeof(magic));

        if (magic == GROUP_VER_MAGIC)
        {
            f.read((uint8_t *)&groupLamport, sizeof(groupLamport));
            f.read((uint8_t *)relayVer, sizeof(relayVer));
        }
    }

    f.close();
}

void groupInit()
{
    memset(groupPending, 0, sizeof(groupPending));
    groupApplyingRemote = false;

    groupLoadMembers();
    groupLoadVersions();
}

//------------------------------------------------------
//                  صف ارسال (Unicast + ACK/Retry)
//------------------------------------------------------
bool groupEnqueue(uint8_t peerIndex, uint32_t group, bool state, uint32_t ts, uint32_t origin)
{
    if (peerIndex >= MAX_PEERS || !peers[peerIndex].used)
        return false;

    int slot = -1;

    // برای همین Peer+گروه، فرمان جدید جای فرمان Pending قبلی را می‌گیرد
    for (int i = 0; i < GROUP_MAX_PENDING; i++)
    {
        if (groupPending[i].used &&
            groupPending[i].peerIndex == peerIndex &&
            groupPending[i].group == group)
        {
            slot = i;
            break;
        }
    }

    if (slot < 0)
    {
        for (int i = 0; i < GROUP_MAX_PENDING; i++)
        {
            if (!groupPending[i].used)
            {
                slot = i;
                break;
            }
        }
    }

    if (slot < 0)
    {
        if (debug)
            DBG_PRINTLN("Group: pending queue full");

        return false;
    }

    uint16_t cmdId = nextCommandId++;
    if (nextCommandId == 0)
        nextCommandId = 1;

    GroupPending &g = groupPending[slot];

    g.used         = true;
    g.commandId    = cmdId;
    g.peerIndex    = peerIndex;
    g.state        = state;
    g.group        = group;
    g.ts           = ts;
    g.origin       = origin;
    g.lastSentAtMs = 0;
    g.retryCount   = 0;

    return true;
}

void groupSendPending(int i)
{
    GroupPending &g = groupPending[i];

    String body = "GUPD|" + peerLinkDeviceUID() + "|" + String(g.commandId) + "|" +
                  String((unsigned long)g.group) + "|" + (g.state ? "1" : "0") + "|" +
                  String((unsigned long)g.ts) + "|" + String((unsigned long)g.origin);
    String tag = peerLinkSign(body);

    udp.beginPacket(IPAddress(peers[g.peerIndex].ip), UDP_PORT);
    udp.print(body + "|" + tag);
    udp.endPacket();

    g.lastSentAtMs = millis();
}

// از handleAckPacket صدا زده می‌شود
bool groupHandleAck(uint16_t cmdId)
{
    for (int i = 0; i < GROUP_MAX_PENDING; i++)
    {
        if (groupPending[i].used && groupPending[i].commandId == cmdId)
        {
            groupPending[i].used = false;
            return true;
        }
    }

    return false;
}

// از taskPeerLink صدا زده می‌شود (غیر-بلاکینگ)
void taskGroupPending()
{
    unsigned long now = millis();

    for (int i = 0; i < GROUP_MAX_PENDING; i++)
    {
        GroupPending &g = groupPending[i];

        if (!g.used)
            continue;

        if (g.lastSentAtMs == 0)
        {
            groupSendPending(i);
            continue;
        }

        if (now - g.lastSentAtMs < GROUP_ACK_TIMEOUT_MS)
            continue;

        if (g.retryCount >= GROUP_MAX_RETRIES)
        {
            if (debug)
            {
                DBG_PRINT("Group: update ");
                DBG_PRINT(g.commandId);
                DBG_PRINTLN(" FAILED (no ACK)");
            }

            g.used = false;
            continue;
        }

        g.retryCount++;
        groupSendPending(i);
    }
}

//------------------------------------------------------
//                  انتشار و اعمال تغییر
//------------------------------------------------------
void groupSendToPeers(uint32_t group, bool on, uint32_t ts, uint32_t origin)
{
    for (uint8_t p = 0; p < MAX_PEERS; p++)
    {
        if (!peers[p].used || !isPeerOnline(p))
            continue;

        groupEnqueue(p, group, on, ts, origin);
    }
}

void groupPublish(uint8_t relay, uint32_t skipGroup, bool useSkip);

// تغییر گروه را روی کلیدهای «همین دستگاه» که عضو گروه‌اند اعمال می‌کند (اگر برچسب جدیدتر باشد)
void groupApplyToLocal(uint32_t group, bool on, uint32_t ts, uint32_t origin, int exceptRelay)
{
    for (uint8_t r = 0; r < TOUCH_COUNT; r++)
    {
        if ((int)r == exceptRelay)
            continue;

        if (groupFindMember(r, group) < 0)
            continue;

        if (!groupVerNewer(ts, origin, relayVer[r].ts, relayVer[r].origin))
            continue;

        bool before = relayGetState(r);

        groupApplyingRemote = true;

        if (on)
            relayOn(r, false);
        else
            relayOff(r, false);

        groupApplyingRemote = false;

        relayVer[r].ts     = ts;
        relayVer[r].origin = origin;

        if (debug)
        {
            DBG_PRINT("Group: relay ");
            DBG_PRINT(r);
            DBG_PRINT(" <- group ");
            DBG_PRINT((unsigned long)group);
            DBG_PRINTLN(on ? " ON" : " OFF");
        }

        if (relayGetState(r) != before)
        {
            peerChangedRelay[r] = true;   // Rule آینه‌ی قدیمی (در صورت وجود) آن را برنگرداند

            // اگر این کلید در گروه‌های دیگری فرمان‌ده است، تغییر را آن‌جا هم منتشر کن
            groupPublish(r, group, true);
        }
    }
}

// تغییر فعلیِ رله (با برچسب فعلی‌اش) را برای همه‌ی گروه‌هایی که این کلید در آن‌ها فرمان‌ده است منتشر می‌کند
void groupPublish(uint8_t relay, uint32_t skipGroup, bool useSkip)
{
    if (relay >= TOUCH_COUNT)
        return;

    bool     on     = relayGetState(relay);
    uint32_t ts     = relayVer[relay].ts;
    uint32_t origin = relayVer[relay].origin;

    for (uint8_t m = 0; m < groupMemberCount[relay]; m++)
    {
        if (groupMembers[relay][m].role != GROUP_ROLE_COMMANDER)
            continue;

        uint32_t g = groupMembers[relay][m].group;

        if (useSkip && g == skipGroup)
            continue;

        groupSendToPeers(g, on, ts, origin);
        groupApplyToLocal(g, on, ts, origin, relay);
    }
}

// از relayOn / relayOff / relayToggle (فایل اصلی) بعد از هر تغییر واقعی رله صدا زده می‌شود
void groupOnRelayChanged(uint8_t relay)
{
    if (relay >= TOUCH_COUNT)
        return;

    if (groupApplyingRemote)
        return;   // تغییر ناشی از گروه است و برچسبش از خود بسته می‌آید

    // فقط کلید فرمان‌ده برچسب می‌گیرد و منتشر می‌کند. تغییر محلیِ یک شنونده نباید برچسبی
    // بگیرد که بعداً جلوی فرمان‌های گروه را بگیرد.
    if (groupCommanderCount(relay) == 0)
        return;

    groupLamport++;
    relayVer[relay].ts     = groupLamport;
    relayVer[relay].origin = groupNodeId();

    groupPublish(relay, 0, false);
}

// بعد از ریست/آنلاین شدن یک Peer: وضعیت و برچسب فعلی کلیدهای فرمان‌ده برای او فرستاده می‌شود.
// برچسب قدیمی‌تر در گیرنده رد می‌شود؛ پس این Push هیچ‌وقت وضعیت جدیدتر را خراب نمی‌کند.
void groupResyncPeer(uint8_t peerIdx)
{
    for (uint8_t r = 0; r < TOUCH_COUNT; r++)
    {
        if (groupCommanderCount(r) == 0)
            continue;

        // وضعیت موقت رله‌ی زمان‌محور قبل از ساعت معتبر + Catch-up مرجع نیست
        if (relayTimeDependent[r] && !timeStateReady())
            continue;

        bool on = relayGetState(r);

        for (uint8_t m = 0; m < groupMemberCount[r]; m++)
        {
            if (groupMembers[r][m].role != GROUP_ROLE_COMMANDER)
                continue;

            groupEnqueue(peerIdx, groupMembers[r][m].group, on, relayVer[r].ts, relayVer[r].origin);
        }
    }
}

//------------------------------------------------------
//                  دریافت GUPD
//------------------------------------------------------
void handleGroupUpdatePacket(const String &pkt, IPAddress remoteIP)
{
    String parts[8];
    int n = peerLinkSplit(pkt, parts, 8);
    if (n != 8) return;

    String body = parts[0] + "|" + parts[1] + "|" + parts[2] + "|" + parts[3] + "|" +
                  parts[4] + "|" + parts[5] + "|" + parts[6];
    String tag  = parts[7];

    if (!peerLinkVerify(body, tag))
        return;

    String   srcId  = parts[1];
    uint16_t cmdId  = (uint16_t)parts[2].toInt();
    uint32_t group  = (uint32_t)strtoul(parts[3].c_str(), nullptr, 10);
    bool     on     = (parts[4] == "1");
    uint32_t ts     = (uint32_t)strtoul(parts[5].c_str(), nullptr, 10);
    uint32_t origin = (uint32_t)strtoul(parts[6].c_str(), nullptr, 10);

    if (srcId == peerLinkDeviceUID())
        return;

    // IP و آنلاین بودن فرستنده را به‌روز کن (و اگر ناشناس بود، اضافه‌اش کن)
    int idx = peerLinkLearnPeer(srcId, remoteIP);

    if (idx >= 0)
    {
        peers[idx].ip = (uint32_t)remoteIP;
        peers[idx].lastSeenMs = millis();
    }

    // ساعت منطقی را با همه‌ی رویدادهای شبکه هم‌گام نگه می‌داریم (حتی اگر عضو گروه نیستیم)
    if (ts > groupLamport)
        groupLamport = ts;

    if (group != 0)
        groupApplyToLocal(group, on, ts, origin, -1);

    // همیشه ACK (چه اعمال شد، چه قدیمی/تکراری بود، چه عضو گروه نبودیم)
    String ackBody = "ACK|" + peerLinkDeviceUID() + "|" + String(cmdId) + "|OK";
    String ackTag  = peerLinkSign(ackBody);

    udp.beginPacket(remoteIP, UDP_PORT);
    udp.print(ackBody + "|" + ackTag);
    udp.endPacket();
}

//------------------------------------------------------
//                  وب: تنظیم عضویت‌ها
//------------------------------------------------------
//  /setGroup?relay=0&group=7&role=1     (role: 0=شنونده، 1=فرمان‌ده)
//  /delGroup?relay=0&group=7
//  /getGroups
//------------------------------------------------------
void handleSetGroup()
{
    if (!server.hasArg("relay") || !server.hasArg("group") || !server.hasArg("role"))
    {
        server.send(400, "text/plain", "Missing relay / group / role");
        return;
    }

    int relay = server.arg("relay").toInt();
    int role  = server.arg("role").toInt();
    uint32_t group = (uint32_t)strtoul(server.arg("group").c_str(), nullptr, 10);

    if (relay < 0 || relay >= TOUCH_COUNT)
    {
        server.send(400, "text/plain", "Bad relay index");
        return;
    }

    if (group == 0)
    {
        server.send(400, "text/plain", "Group must be a positive number");
        return;
    }

    if (role != GROUP_ROLE_LISTENER && role != GROUP_ROLE_COMMANDER)
    {
        server.send(400, "text/plain", "Bad role (0=listener, 1=commander)");
        return;
    }

    int existing = groupFindMember((uint8_t)relay, group);

    // هر کلید فقط یک عضویت فرمان‌ده می‌تواند داشته باشد
    if (role == GROUP_ROLE_COMMANDER)
    {
        for (uint8_t i = 0; i < groupMemberCount[relay]; i++)
        {
            if (groupMembers[relay][i].role == GROUP_ROLE_COMMANDER &&
                groupMembers[relay][i].group != group)
            {
                server.send(409, "text/plain", "Relay is already a commander of another group");
                return;
            }
        }
    }

    if (existing >= 0)
    {
        groupMembers[relay][existing].role = (uint8_t)role;
    }
    else
    {
        if (groupMemberCount[relay] >= GROUP_MAX_MEMBERSHIPS)
        {
            server.send(507, "text/plain", "Too many memberships for this relay");
            return;
        }

        groupMembers[relay][groupMemberCount[relay]].group = group;
        groupMembers[relay][groupMemberCount[relay]].role  = (uint8_t)role;
        groupMemberCount[relay]++;
    }

    groupSaveMembers();

    server.send(200, "application/json", "{\"ok\":1}");
}

void handleDelGroup()
{
    if (!server.hasArg("relay") || !server.hasArg("group"))
    {
        server.send(400, "text/plain", "Missing relay / group");
        return;
    }

    int relay = server.arg("relay").toInt();
    uint32_t group = (uint32_t)strtoul(server.arg("group").c_str(), nullptr, 10);

    if (relay < 0 || relay >= TOUCH_COUNT)
    {
        server.send(400, "text/plain", "Bad relay index");
        return;
    }

    int idx = groupFindMember((uint8_t)relay, group);

    if (idx < 0)
    {
        server.send(404, "text/plain", "Membership not found");
        return;
    }

    for (uint8_t i = (uint8_t)idx; i + 1 < groupMemberCount[relay]; i++)
        groupMembers[relay][i] = groupMembers[relay][i + 1];

    groupMemberCount[relay]--;
    memset(&groupMembers[relay][groupMemberCount[relay]], 0, sizeof(GroupMembership));

    groupSaveMembers();

    server.send(200, "application/json", "{\"ok\":1}");
}

void handleGetGroups()
{
    String json;
    json.reserve(256);
    json += "{\"node\":";
    json += (unsigned long)groupNodeId();
    json += ",\"lamport\":";
    json += (unsigned long)groupLamport;
    json += ",\"relays\":[";

    for (uint8_t r = 0; r < TOUCH_COUNT; r++)
    {
        if (r) json += ',';

        json += "{\"relay\":";   json += r;
        json += ",\"state\":";   json += (relayGetState(r) ? 1 : 0);
        json += ",\"ts\":";      json += (unsigned long)relayVer[r].ts;
        json += ",\"origin\":";  json += (unsigned long)relayVer[r].origin;
        json += ",\"groups\":[";

        for (uint8_t i = 0; i < groupMemberCount[r]; i++)
        {
            if (i) json += ',';

            json += "{\"group\":";  json += (unsigned long)groupMembers[r][i].group;
            json += ",\"role\":";   json += groupMembers[r][i].role;
            json += '}';
        }

        json += "]}";
    }

    json += "]}";

    server.send(200, "application/json", json);
}
