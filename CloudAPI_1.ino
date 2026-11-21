//======================================================
//        CLOUD / CUSTOM SERVER SYNC (asynchronous HTTP)
//======================================================

//SETSECRET=2F2FF96C14A2FA60FEB976349BD0FAF411349144C038D4E9812EE521968B4D25 // MAC:48:3F:DA:5D:02:C3
//SETSECRET=DCEAF19832B2D37AA3B11E7CC2A09BF33CDA77953226EF437D05048A97E38A31 // MAC:48:3F:DA:19:FB:A9

// Cloud report uses HTTP POST with a compact binary status packet.
// Relay state mutations remain on the existing seton/setoff endpoints for now.
// The POST response body must retain the existing plain-text relay status format.
//
// Dependencies:
//   ESP8266: ESPAsyncTCP (ESP32Async organization)
//   ESP32-C3: AsyncTCP (ESP32Async organization)
//
// The server must return a short plain-text status body from /status/,
// e.g. "0100". This client sends HTTP/1.0 with Connection: close.

static bool cloudCalculateHMAC(
    const uint8_t* key,
    size_t keyLength,
    const uint8_t* data,
    size_t dataLength,
    uint8_t output[32])
{
#if MODULE_TYPE == MODULE_ESP12F

    br_hmac_key_context keyContext;
    br_hmac_context context;

    br_hmac_key_init(
        &keyContext,
        &br_sha256_vtable,
        key,
        keyLength
    );

    br_hmac_init(&context, &keyContext, 0);
    br_hmac_update(&context, data, dataLength);
    br_hmac_out(&context, output);

    return true;

#elif MODULE_TYPE == MODULE_ESP32C3

    const mbedtls_md_info_t* mdInfo =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    if (mdInfo == nullptr)
        return false;

    return mbedtls_md_hmac(
        mdInfo,
        key,
        keyLength,
        data,
        dataLength,
        output
    ) == 0;

#else
    return false;
#endif
}

// تست استاندارد RFC 4231
static bool cloudHMACTest()
{
    uint8_t key[20];
    memset(key, 0x0B, sizeof(key));

    const uint8_t data[] = "Hi There";

    const uint8_t expected[32] = {
        0xB0,0x34,0x4C,0x61,0xD8,0xDB,0x38,0x53,
        0x5C,0xA8,0xAF,0xCE,0xAF,0x0B,0xF1,0x2B,
        0x88,0x1D,0xC2,0x00,0xC9,0x83,0x3D,0xA7,
        0x26,0xE9,0x37,0x6C,0x2E,0x32,0xCF,0xF7
    };

    uint8_t result[32];

    if (!cloudCalculateHMAC(
        key, sizeof(key),
        data, sizeof(data) - 1,
        result))
        return false;

    return memcmp(result, expected, 32) == 0;
}
//======================================================
// SERVER
//======================================================

static const char* CLOUD_HOST = "net-io.ir"; //"hoolhelpsc.ir";
static const uint16_t CLOUD_PORT = 80;
static const char* CLOUD_STATUS_PATH = "/kp4000/test.php";


//======================================================
// DEVICE SECRET STORAGE
//======================================================

#define CLOUD_SECRET_SIZE 32
#define CLOUD_SECRET_FILE "/device_secret.bin"

static uint8_t cloudDeviceSecret[CLOUD_SECRET_SIZE] = {0};
static bool cloudDeviceSecretValid = false;

//======================================================
// PERSISTENT 64-BIT SEQUENCE COUNTER
//======================================================

#define CLOUD_COUNTER_FILE0 "/counter0.dat"
#define CLOUD_COUNTER_FILE1 "/counter1.dat"

#define CLOUD_COUNTER_MAGIC   0x43545231UL
#define CLOUD_COUNTER_VERSION 1
#define CLOUD_COUNTER_BLOCK   1024ULL

// struct __attribute__((packed)) CloudCounterSlot
// {
//     uint32_t magic;
//     uint16_t version;
//     uint16_t reserved;
//     uint32_t generation;
//     uint64_t reservedUntil;
//     uint32_t crc32;
// };

static uint64_t cloudCounterNext = 0;
static uint64_t cloudCounterEnd = 0;
static bool cloudCounterHasNext = false;
static bool cloudCounterStorageReady = false;
static char cloudRequestCounterText[21] = {0};
//------------------------------------------------------
// CRC32
//------------------------------------------------------

static uint32_t cloudCounterCRC32(const uint8_t* data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;

    for (size_t i = 0; i < length; i++)
    {
        crc ^= data[i];

        for (uint8_t bit = 0; bit < 8; bit++)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320UL;
            else
                crc >>= 1;
        }
    }

    return ~crc;
}

//======================================================
// PERSISTENT SERVER COMMAND JOURNAL
//======================================================

#define CLOUD_CMD_FILE0 "/cmd0.dat"
#define CLOUD_CMD_FILE1 "/cmd1.dat"

#define CLOUD_CMD_MAGIC   0x434D4431UL
#define CLOUD_CMD_VERSION 1


// struct CloudCommandJournal;

// struct __attribute__((packed)) CloudCommandJournal
// {
    // uint32_t magic;
    // uint16_t version;
    // uint16_t reserved;
    // uint32_t generation;

    // uint32_t appliedVersion[TOUCH_COUNT];
    // uint32_t pendingVersion[TOUCH_COUNT];

    // uint8_t pendingState[TOUCH_COUNT];
    // uint8_t pendingFlags[TOUCH_COUNT];

    // uint32_t crc32;
// };

static CloudCommandJournal cloudCommandJournal;
static uint8_t cloudCommandActiveSlot = 1;
static bool cloudCommandJournalReady = false;

//------------------------------------------------------
// READ JOURNAL SLOT
//------------------------------------------------------

static bool cloudCommandReadSlot(
    uint8_t index,
    CloudCommandJournal &slot)
{
    const char* path =
        (index == 0) ? CLOUD_CMD_FILE0 : CLOUD_CMD_FILE1;

    File file = LittleFS.open(path, "r");

    if (!file)
        return false;

    if (file.size() != sizeof(CloudCommandJournal))
    {
        file.close();
        return false;
    }

    size_t length = file.read(
        (uint8_t*)&slot,
        sizeof(CloudCommandJournal)
    );

    file.close();

    if (length != sizeof(CloudCommandJournal))
        return false;

    if (slot.magic != CLOUD_CMD_MAGIC ||
        slot.version != CLOUD_CMD_VERSION)
        return false;

    uint32_t expectedCRC = cloudCounterCRC32(
        (const uint8_t*)&slot,
        sizeof(CloudCommandJournal) - sizeof(slot.crc32)
    );

    return slot.crc32 == expectedCRC;
}

//------------------------------------------------------
// WRITE JOURNAL SLOT
//------------------------------------------------------

static bool cloudCommandWriteSlot(
    uint8_t index,
    const CloudCommandJournal &slot)
{
    const char* path =
        (index == 0) ? CLOUD_CMD_FILE0 : CLOUD_CMD_FILE1;

    File file = LittleFS.open(path, "w");

    if (!file)
        return false;

    size_t written = file.write(
        (const uint8_t*)&slot,
        sizeof(CloudCommandJournal)
    );

    file.flush();
    file.close();

    if (written != sizeof(CloudCommandJournal))
        return false;

    CloudCommandJournal verify;

    if (!cloudCommandReadSlot(index, verify))
        return false;

    return memcmp(&slot, &verify, sizeof(slot)) == 0;
}

//------------------------------------------------------
// SAVE JOURNAL
//------------------------------------------------------

static bool cloudCommandSave()
{
    if (cloudCommandJournal.generation == 0xFFFFFFFFUL)
        return false;

    CloudCommandJournal next = cloudCommandJournal;

    next.generation++;

    next.crc32 = cloudCounterCRC32(
        (const uint8_t*)&next,
        sizeof(CloudCommandJournal) - sizeof(next.crc32)
    );

    uint8_t targetSlot = 1 - cloudCommandActiveSlot;

    if (!cloudCommandWriteSlot(targetSlot, next))
        return false;

    cloudCommandJournal = next;
    cloudCommandActiveSlot = targetSlot;

    return true;
}

//------------------------------------------------------
// INITIALIZE JOURNAL
//------------------------------------------------------

static bool cloudCommandJournalInit()
{
    cloudCommandJournalReady = false;

    CloudCommandJournal slot0;
    CloudCommandJournal slot1;

    bool valid0 = cloudCommandReadSlot(0, slot0);
    bool valid1 = cloudCommandReadSlot(1, slot1);

    bool exists0 = LittleFS.exists(CLOUD_CMD_FILE0);
    bool exists1 = LittleFS.exists(CLOUD_CMD_FILE1);
if (valid0 && valid1)
    {
        if (slot1.generation > slot0.generation)
        {
            cloudCommandJournal = slot1;
            cloudCommandActiveSlot = 1;
        }
        else
        {
            cloudCommandJournal = slot0;
            cloudCommandActiveSlot = 0;
        }
    }
    else if (valid0)
    {
        cloudCommandJournal = slot0;
        cloudCommandActiveSlot = 0;
    }
    else if (valid1)
    {
        cloudCommandJournal = slot1;
        cloudCommandActiveSlot = 1;
    }
    else
    {
        // فقط در اولین راه‌اندازی مجاز به ساخت Journal جدید هستیم.
        if (exists0 || exists1)
        {
            DBG_PRINTLN("[Cloud] Command Journal: CORRUPTED");
            return false;
        }

        memset(&cloudCommandJournal, 0,
               sizeof(cloudCommandJournal));

        cloudCommandJournal.magic = CLOUD_CMD_MAGIC;
        cloudCommandJournal.version = CLOUD_CMD_VERSION;
        cloudCommandJournal.generation = 0;

        cloudCommandActiveSlot = 1;

        if (!cloudCommandSave())
            return false;
    }

    cloudCommandJournalReady = true;

    DBG_PRINTLN("[Cloud] Command Journal: READY");

    return true;
}

//------------------------------------------------------
// BEGIN SERVER COMMAND
// ذخیره فرمان قبل از اجرای آن
//------------------------------------------------------

static bool cloudCommandBegin(
    uint8_t channel,
    uint32_t version,
    bool state)
{
    if (!cloudCommandJournalReady ||
        channel >= TOUCH_COUNT ||
        version == 0)
        return false;

    uint32_t latestVersion =
        cloudCommandJournal.appliedVersion[channel];

    if (cloudCommandJournal.pendingFlags[channel] &&
        cloudCommandJournal.pendingVersion[channel] > latestVersion)
    {
        latestVersion =
            cloudCommandJournal.pendingVersion[channel];
    }

    if (version <= latestVersion)
        return false;

    CloudCommandJournal backup = cloudCommandJournal;

    cloudCommandJournal.pendingVersion[channel] = version;
    cloudCommandJournal.pendingState[channel] = state ? 1 : 0;
    cloudCommandJournal.pendingFlags[channel] = 1;

    if (!cloudCommandSave())
    {
        cloudCommandJournal = backup;
        return false;
    }

    return true;
}

//------------------------------------------------------
// COMMIT SERVER COMMAND
// ثبت نسخه پس از اجرای موفق فرمان
//------------------------------------------------------

static bool cloudCommandCommit(uint8_t channel)
{
    if (!cloudCommandJournalReady ||
        channel >= TOUCH_COUNT ||
        !cloudCommandJournal.pendingFlags[channel])
        return false;

    CloudCommandJournal backup = cloudCommandJournal;

    cloudCommandJournal.appliedVersion[channel] =
        cloudCommandJournal.pendingVersion[channel];

    cloudCommandJournal.pendingFlags[channel] = 0;

    if (!cloudCommandSave())
    {
        cloudCommandJournal = backup;
        return false;
    }

    return true;
}

static bool cloudCommandGetPending(
    uint8_t channel,
    uint32_t &version,
    bool &state)
{
    if (!cloudCommandJournalReady ||
        channel >= TOUCH_COUNT ||
        !cloudCommandJournal.pendingFlags[channel])
        return false;

    version = cloudCommandJournal.pendingVersion[channel];
    state = cloudCommandJournal.pendingState[channel] != 0;

    return version != 0;
}

static uint32_t cloudCommandGetApplied(uint8_t channel)
{
    if (!cloudCommandJournalReady || channel >= TOUCH_COUNT)
        return 0;

    return cloudCommandJournal.appliedVersion[channel];
}
//------------------------------------------------------
// UINT64 TO DECIMAL STRING
//------------------------------------------------------

static void cloudUint64ToDecimal(uint64_t value, char output[21])
{
    char temp[20];
    uint8_t length = 0;

    do
    {
        temp[length++] = '0' + (value % 10);
        value /= 10;
    }
    while (value != 0 && length < sizeof(temp));

    for (uint8_t i = 0; i < length; i++)
        output[i] = temp[length - 1 - i];

    output[length] = '\0';
}

//------------------------------------------------------
// READ COUNTER SLOT
//------------------------------------------------------

static bool cloudReadCounterSlot(
    uint8_t index,
    CloudCounterSlot& slot)
{
    const char* path =
        (index == 0) ? CLOUD_COUNTER_FILE0 : CLOUD_COUNTER_FILE1;

    File file = LittleFS.open(path, "r");

    if (!file)
        return false;

    if (file.size() != sizeof(CloudCounterSlot))
    {
        file.close();
        return false;
    }

    size_t length = file.read(
        (uint8_t*)&slot,
        sizeof(CloudCounterSlot)
    );

    file.close();

    if (length != sizeof(CloudCounterSlot))
        return false;

    if (slot.magic != CLOUD_COUNTER_MAGIC ||
        slot.version != CLOUD_COUNTER_VERSION ||
        slot.reservedUntil == 0)
        return false;

    uint32_t expectedCRC = cloudCounterCRC32(
        (const uint8_t*)&slot,
        sizeof(CloudCounterSlot) - sizeof(slot.crc32)
    );

    return slot.crc32 == expectedCRC;
}

//------------------------------------------------------
// WRITE AND VERIFY COUNTER SLOT
//------------------------------------------------------

static bool cloudWriteCounterSlot(
    uint8_t index,
    const CloudCounterSlot& slot)
{
    const char* path =
        (index == 0) ? CLOUD_COUNTER_FILE0 : CLOUD_COUNTER_FILE1;

    File file = LittleFS.open(path, "w");

    if (!file)
        return false;

    size_t written = file.write(
        (const uint8_t*)&slot,
        sizeof(CloudCounterSlot)
    );

    file.flush();
    file.close();

    if (written != sizeof(CloudCounterSlot))
        return false;

    CloudCounterSlot verify;

    if (!cloudReadCounterSlot(index, verify))
        return false;

    return memcmp(&slot, &verify, sizeof(slot)) == 0;
}
//------------------------------------------------------
// RESERVE A NEW COUNTER BLOCK
//------------------------------------------------------

static bool cloudCounterReserveBlock()
{
    CloudCounterSlot slot0;
    CloudCounterSlot slot1;

    bool valid0 = cloudReadCounterSlot(0, slot0);
    bool valid1 = cloudReadCounterSlot(1, slot1);

    uint64_t start;
    uint64_t end;
    uint32_t generation;
    uint8_t targetSlot;

    const uint64_t maxCounter = ~(uint64_t)0;

    // First initialization
    if (!valid0 && !valid1)
    {
        start = 1;
        end = CLOUD_COUNTER_BLOCK;
        generation = 1;
        targetSlot = 0;
    }
    else
    {
        // Select the newest valid slot
        uint8_t activeSlot;

        if (valid0 && valid1)
        {
            if (slot1.generation > slot0.generation)
                activeSlot = 1;
            else if (slot0.generation > slot1.generation)
                activeSlot = 0;
            else
                activeSlot =
                    (slot1.reservedUntil > slot0.reservedUntil) ? 1 : 0;
        }
        else
        {
            activeSlot = valid0 ? 0 : 1;
        }

        const CloudCounterSlot& active =
            (activeSlot == 0) ? slot0 : slot1;

        if (active.generation == UINT32_MAX ||
            active.reservedUntil == maxCounter)
        {
            return false;
        }

        start = active.reservedUntil + 1;
        generation = active.generation + 1;
        targetSlot = 1 - activeSlot;

        uint64_t remaining = maxCounter - active.reservedUntil;
        uint64_t blockSize = CLOUD_COUNTER_BLOCK;

        if (remaining < blockSize)
            blockSize = remaining;

        if (blockSize == 0)
            return false;

        end = active.reservedUntil + blockSize;
    }

    CloudCounterSlot newSlot;
    memset(&newSlot, 0, sizeof(newSlot));

    newSlot.magic = CLOUD_COUNTER_MAGIC;
    newSlot.version = CLOUD_COUNTER_VERSION;
    newSlot.reserved = 0;
    newSlot.generation = generation;
    newSlot.reservedUntil = end;

    newSlot.crc32 = cloudCounterCRC32(
        (const uint8_t*)&newSlot,
        sizeof(CloudCounterSlot) - sizeof(newSlot.crc32)
    );

    // The new block is not usable until the write is verified.
    if (!cloudWriteCounterSlot(targetSlot, newSlot))
    {
        cloudCounterStorageReady = false;
        cloudCounterHasNext = false;
        return false;
    }

    cloudCounterNext = start;
    cloudCounterEnd = end;
    cloudCounterHasNext = true;
    cloudCounterStorageReady = true;

    return true;
}

static bool cloudCounterAdvancePast(uint64_t serverCounter)
{
    if (!cloudCounterStorageReady)
        return false;

    if (cloudCounterHasNext &&
        cloudCounterNext > serverCounter)
        return true;

    CloudCounterSlot slot0;
    CloudCounterSlot slot1;

    bool valid0 = cloudReadCounterSlot(0, slot0);
    bool valid1 = cloudReadCounterSlot(1, slot1);

    uint64_t base = serverCounter;
    uint32_t generation = 1;
    uint8_t targetSlot = 0;

    if (valid0 || valid1)
    {
        uint8_t activeSlot;
  if (valid0 && valid1)
        {
            if (slot1.generation > slot0.generation)
                activeSlot = 1;
            else if (slot0.generation > slot1.generation)
                activeSlot = 0;
            else
                activeSlot =
                    (slot1.reservedUntil > slot0.reservedUntil) ? 1 : 0;
        }
        else
        {
            activeSlot = valid0 ? 0 : 1;
        }

        const CloudCounterSlot& active =
            (activeSlot == 0) ? slot0 : slot1;

        if (active.generation == UINT32_MAX)
            return false;

        if (active.reservedUntil > base)
            base = active.reservedUntil;

        generation = active.generation + 1;
        targetSlot = 1 - activeSlot;
    }

    const uint64_t maxCounter = ~(uint64_t)0;

    if (base > maxCounter - CLOUD_COUNTER_BLOCK)
        return false;

    uint64_t start = base + 1;
    uint64_t end = base + CLOUD_COUNTER_BLOCK;

    CloudCounterSlot newSlot;
    memset(&newSlot, 0, sizeof(newSlot));

    newSlot.magic = CLOUD_COUNTER_MAGIC;
    newSlot.version = CLOUD_COUNTER_VERSION;
    newSlot.generation = generation;
    newSlot.reservedUntil = end;

    newSlot.crc32 = cloudCounterCRC32(
        (const uint8_t*)&newSlot,
        sizeof(CloudCounterSlot) - sizeof(newSlot.crc32)
    );

    if (!cloudWriteCounterSlot(targetSlot, newSlot))
    {
        cloudCounterStorageReady = false;
        cloudCounterHasNext = false;
        return false;
    }

    cloudCounterNext = start;
    cloudCounterEnd = end;
    cloudCounterHasNext = true;

    char startText[21];
    char endText[21];

    cloudUint64ToDecimal(start, startText);
    cloudUint64ToDecimal(end, endText);

    DBG_PRINT("[Crypto] Counter recovered; new block: ");
    DBG_PRINT(startText);
    DBG_PRINT(" .. ");
    DBG_PRINTLN(endText);

    return true;
}

static bool cloudParseCounterDecimal(
    const String& text,
    uint64_t& value)
{
    if (text.length() == 0 || text.length() > 20)
        return false;

    value = 0;

    const uint64_t maxValue = ~(uint64_t)0;

    for (size_t i = 0; i < text.length(); i++)
    {
        char c = text[i];

        if (c < '0' || c > '9')
            return false;

        uint8_t digit = c - '0';

        if (value > (maxValue - digit) / 10)
            return false;

        value = value * 10 + digit;
    }

    if (text.length() > 1 && text[0] == '0')
        return false;

    return true;
}

static bool cloudRecoverCounterFromResponse(const String& body)
{
    const String key = "LAST_COUNTER=";

    int pos = body.indexOf(key);

    if (pos < 0)
        return false;

    pos += key.length();

    int end = body.indexOf(';', pos);

    String value = (end < 0)
        ? body.substring(pos)
        : body.substring(pos, end);

    uint64_t serverCounter;

    if (!cloudParseCounterDecimal(value, serverCounter))
        return false;

    return cloudCounterAdvancePast(serverCounter);
}


//------------------------------------------------------
// GET NEXT UNIQUE COUNTER
//------------------------------------------------------

static bool cloudGetNextCounter(uint64_t& counter)
{
    if (!cloudCounterStorageReady)
        return false;

    if (!cloudCounterHasNext)
    {
        if (!cloudCounterReserveBlock())
            return false;
    }

    counter = cloudCounterNext;

    if (cloudCounterNext == cloudCounterEnd)
    {
        cloudCounterHasNext = false;
    }
    else
    {
        cloudCounterNext++;
    }

    return true;
}

//------------------------------------------------------
// COUNTER SELF TEST
//------------------------------------------------------

static bool cloudCounterSelfTest()
{
    uint64_t a, b, c;

    if (!cloudGetNextCounter(a) ||
        !cloudGetNextCounter(b) ||
        !cloudGetNextCounter(c))
        return false;

    if (a == UINT64_MAX ||
        b != a + 1 ||
        c != b + 1)
        return false;

    char textA[21];
    char textB[21];
    char textC[21];

    cloudUint64ToDecimal(a, textA);
    cloudUint64ToDecimal(b, textB);
    cloudUint64ToDecimal(c, textC);

    DBG_PRINT("[Crypto] Counter self-test: ");
    DBG_PRINT(textA);
    DBG_PRINT(",");
    DBG_PRINT(textB);
    DBG_PRINT(",");
    DBG_PRINTLN(textC);

    return true;
}

//------------------------------------------------------
// INITIALIZE COUNTER STORAGE
//------------------------------------------------------
static bool cloudInitSequenceCounter()
{
    cloudCounterStorageReady = false;
    cloudCounterHasNext = false;

    if (!cloudCounterReserveBlock())
    {
        DBG_PRINTLN("[Crypto] Counter storage: FAILED");
        return false;
    }

    char startText[21];
    char endText[21];

    cloudUint64ToDecimal(cloudCounterNext, startText);
    cloudUint64ToDecimal(cloudCounterEnd, endText);

    DBG_PRINT("[Crypto] Counter reserved: ");
    DBG_PRINT(startText);
    DBG_PRINT(" .. ");
    DBG_PRINTLN(endText);

    if (!cloudCounterSelfTest())
    {
        cloudCounterStorageReady = false;
        cloudCounterHasNext = false;

        DBG_PRINTLN("[Crypto] Counter self-test: FAILED");
        return false;
    }

    DBG_PRINTLN("[Crypto] Counter storage: READY");

    return true;
}


// Load device secret from LittleFS
static bool cloudLoadDeviceSecret()
{
    cloudDeviceSecretValid = false;

    File file = LittleFS.open(CLOUD_SECRET_FILE, "r");

    if (!file)
        return false;

    if (file.size() != CLOUD_SECRET_SIZE)
    {
        file.close();
        return false;
    }

    size_t readLength = file.read(
        cloudDeviceSecret,
        CLOUD_SECRET_SIZE
    );

    file.close();

    if (readLength != CLOUD_SECRET_SIZE)
        return false;

    cloudDeviceSecretValid = true;
    return true;
}

//================================
#define CLOUD_EVENT_FILE "/cloud_events.dat"

static uint8_t cloudEventFlags = 0;
static uint8_t cloudRequestEventFlags = 0;

static void cloudPersistEventFlags()
{
    File file = LittleFS.open(CLOUD_EVENT_FILE, "w");

    if (!file)
        return;

    file.write(&cloudEventFlags, 1);
    file.flush();
    file.close();
}

static void cloudLoadEventFlags()
{
    cloudEventFlags = 0;

    File file = LittleFS.open(CLOUD_EVENT_FILE, "r");

    if (file)
    {
        if (file.size() == 1)
            file.read(&cloudEventFlags, 1);

        file.close();
    }

    cloudEventFlags &= 0xF0;
}

void cloudMarkEventFlag(uint8_t mask)
{
    uint8_t updated = cloudEventFlags | (mask & 0xF0);

    if (updated == cloudEventFlags)
        return;

    cloudEventFlags = updated;
    cloudPersistEventFlags();
}

static void cloudClearEventFlags(uint8_t mask)
{
    uint8_t updated = cloudEventFlags & ~(mask & 0xF0);

    if (updated == cloudEventFlags)
        return;

    cloudEventFlags = updated;
    cloudPersistEventFlags();
}

//======================================================
// SERIAL SECRET PROVISIONING
//======================================================

static int cloudHexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void cloudProcessSecretSerial()
{
    static char line[80];
    static uint8_t lineLength = 0;
    static bool overflow = false;

    while (Serial.available())
    {
        char c = (char)Serial.read();

        if (c == '\r')
            continue;

        if (c == '\n')
        {
            line[lineLength] = '\0';

            if (!overflow &&
                lineLength == 74 &&
                strncmp(line, "SETSECRET=", 10) == 0)
            {
                uint8_t secret[CLOUD_SECRET_SIZE];
                bool valid = true;

                for (uint8_t i = 0; i < CLOUD_SECRET_SIZE; i++)
                {
                    int hi = cloudHexNibble(line[10 + i * 2]);
                    int lo = cloudHexNibble(line[11 + i * 2]);

                    if (hi < 0 || lo < 0)
                    {
                        valid = false;
                        break;
                    }

                    secret[i] = (uint8_t)((hi << 4) | lo);
                }

                if (valid && cloudSaveDeviceSecret(secret))
                    DBG_PRINTLN("[Crypto] Device Secret: SAVED");
                else
                    DBG_PRINTLN("[Crypto] Device Secret: SAVE FAILED");

                memset(secret, 0, sizeof(secret));
            }
            else if (lineLength > 0)
            {
                DBG_PRINTLN("[Crypto] Invalid provisioning command");
            }

            memset(line, 0, sizeof(line));
            lineLength = 0;
            overflow = false;
        }
        else if (!overflow)
        {
            if (lineLength < sizeof(line) - 1)
                line[lineLength++] = c;
            else
                overflow = true;
        }
    }
}
// Save device secret to LittleFS
static bool cloudSaveDeviceSecret(const uint8_t *secret)
{
    if (secret == nullptr)
        return false;

    File file = LittleFS.open(CLOUD_SECRET_FILE, "w");

    if (!file)
        return false;

    size_t written = file.write(
        secret,
        CLOUD_SECRET_SIZE
    );

    file.close();

    if (written != CLOUD_SECRET_SIZE)
        return false;

    memcpy(
        cloudDeviceSecret,
        secret,
        CLOUD_SECRET_SIZE
    );

    cloudDeviceSecretValid = true;
    return true;
}

static const unsigned long CLOUD_POLL_INTERVAL_MS = 10000UL;
static const unsigned long CLOUD_REQUEST_TIMEOUT_MS = 6000UL;
static const unsigned long CLOUD_RETRY_MIN_MS = 1000UL;
static const unsigned long CLOUD_RETRY_MAX_MS = 30000UL;
static const size_t CLOUD_RX_BUFFER_SIZE = 768;
static const size_t CLOUD_STATUS_PACKET_MAX_SIZE =
    12U + 5U + (MAX_TIMERS * 12U) + (TOUCH_COUNT * 4U);

static const size_t CLOUD_REPORT_TEXT_MAX_SIZE =
    192U +
    (MAX_TIMERS * 32U) +
    (4U * ((TOUCH_COUNT + 7U) / 8U)) +
    (TOUCH_COUNT * 8U) +
    16U;

static uint8_t cloudReportBuffer[CLOUD_STATUS_PACKET_MAX_SIZE];
static char cloudReportTextBuffer[CLOUD_REPORT_TEXT_MAX_SIZE];

static size_t cloudReportLength = 0;
static size_t cloudReportTextLength = 0;

//======================================================
// ASYNC REQUEST STATE
//======================================================

enum CloudRequestType : byte
{
    CLOUD_REQ_NONE = 0,
    CLOUD_REQ_POLL,
    CLOUD_REQ_PUSH
};

static AsyncClient cloudClient;
static CloudRequestType cloudRequestType = CLOUD_REQ_NONE;

static bool cloudRequestInFlight = false;
static bool cloudRequestDone = false;
static bool cloudRequestFailed = false;
static unsigned long cloudRequestStartedAt = 0;

static char cloudRequestPath[96] = {0};
static char cloudRxBuffer[CLOUD_RX_BUFFER_SIZE] = {0};
static size_t cloudRxLength = 0;
static int cloudResponseCode = 0;

static byte cloudRequestChannel = 0;
static bool cloudRequestRelayState = false;

static unsigned long cloudLastPollAttempt = 0;
static unsigned long cloudLastPushAttempt = 0;
static unsigned long cloudPollRetryMs = CLOUD_RETRY_MIN_MS;
static unsigned long cloudPushRetryMs = CLOUD_RETRY_MIN_MS;

static bool cloudFirstPoll = true;
static bool cloudSynced = false;
// On the first successful poll after boot, local boot/timer state is authoritative.
static bool cloudBootBaselinePending = true;

// Last state reported by the server. This is deliberately distinct from
// relayState[] so failed writes remain pending.
static bool cloudKnownRelayState[TOUCH_COUNT] = {false};

// Detect local state changes even before the first successful cloud poll.
// This prevents a stale server status from overwriting a local change made
// while the server was unreachable.
static bool cloudObservedRelayState[TOUCH_COUNT] = {false};
static bool cloudObservedInitialized = false;
static bool cloudLocalPending[TOUCH_COUNT] = {false};

/*
  //======================================================
  // SERIAL HTTP PACKET DEBUG HELPERS
  // Diagnostic only: these functions do not modify transmitted/received bytes.
  //======================================================

  static void cloudDebugPrintHex(const uint8_t *data, size_t length)
  {
      for (size_t i = 0; i < length; i++)
      {
          char item[4];
          snprintf(item, sizeof(item), "%02X ", data[i]);
          DBG_PRINT(item);
          if ((i + 1U) % 16U == 0U)
              DBG_PRINTLN("");
      }
      if (length % 16U != 0U)
          DBG_PRINTLN("");
  }

  static void cloudDebugPrintUrlEncoded(const uint8_t *data, size_t length)
  {
      static const char hex[] = "0123456789ABCDEF";
      for (size_t i = 0; i < length; i++)
      {
          const uint8_t c = data[i];
          const bool unreserved =
              (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.' || c == '~';

          if (unreserved)
          {
              char ch[2] = {(char)c, '\0'};
              DBG_PRINT(ch);
          }
          else
          {
              char encoded[4] = {'%', hex[(c >> 4) & 0x0F], hex[c & 0x0F], '\0'};
              DBG_PRINT(encoded);
          }
      }
      DBG_PRINTLN("");
  }
*/
static bool cloudAppendKVText(const char* key, const char* value)
{
    size_t keyLength = strlen(key);
    size_t valueLength = strlen(value);

    size_t needed = keyLength + 1U + valueLength;

    if (cloudReportTextLength > 0)
        needed++;

    if (cloudReportTextLength + needed >= sizeof(cloudReportTextBuffer))
        return false;

    if (cloudReportTextLength > 0)
        cloudReportTextBuffer[cloudReportTextLength++] = '&';

    memcpy(cloudReportTextBuffer + cloudReportTextLength,
           key, keyLength);
    cloudReportTextLength += keyLength;

    cloudReportTextBuffer[cloudReportTextLength++] = '=';

    memcpy(cloudReportTextBuffer + cloudReportTextLength,
           value, valueLength);
    cloudReportTextLength += valueLength;

    cloudReportTextBuffer[cloudReportTextLength] = '\0';

    return true;
}

static bool cloudAppendKVHex(
    const char* key,
    const uint8_t* data,
    size_t length)
{
    static const char hex[] = "0123456789ABCDEF";

    size_t keyLength = strlen(key);
    size_t needed = keyLength + 1U + (length * 2U);

    if (cloudReportTextLength > 0)
        needed++;

    if (cloudReportTextLength + needed >= sizeof(cloudReportTextBuffer))
        return false;

    if (cloudReportTextLength > 0)
        cloudReportTextBuffer[cloudReportTextLength++] = '&';

    memcpy(cloudReportTextBuffer + cloudReportTextLength,
           key, keyLength);
    cloudReportTextLength += keyLength;

    cloudReportTextBuffer[cloudReportTextLength++] = '=';

    for (size_t i = 0; i < length; i++)
    {
        cloudReportTextBuffer[cloudReportTextLength++] =
            hex[(data[i] >> 4) & 0x0F];

        cloudReportTextBuffer[cloudReportTextLength++] =
            hex[data[i] & 0x0F];
    }

    cloudReportTextBuffer[cloudReportTextLength] = '\0';

    return true;
}

static bool cloudBuildKeyValueReport(
    const char* counterText,
    const uint8_t signature[32])
{
    if (counterText == nullptr || signature == nullptr)
        return false;

    cloudReportTextLength = 0;
    cloudReportTextBuffer[0] = '\0';

    const size_t statusBytes = (TOUCH_COUNT + 7U) / 8U;

    const size_t relayOffset = 12U;
    const size_t touchOffset = relayOffset + statusBytes;
    const size_t countOffset = touchOffset + statusBytes;
    const size_t timerOffset = countOffset + 1U;

    const uint8_t timerCount = cloudReportBuffer[countOffset];

    if (cloudReportLength !=
        timerOffset +
        ((size_t)timerCount * 12U) +
        (TOUCH_COUNT * 4U))
        return false;

    if (!cloudAppendKVHex("MAC", cloudReportBuffer, 6))
        return false;

    if (!cloudAppendKVHex("IP", cloudReportBuffer + 6, 4))
        return false;

    if (!cloudAppendKVHex("DEVICE", cloudReportBuffer + 10, 1))
        return false;
        if (!cloudAppendKVHex("DEVICE_INFO_2",cloudReportBuffer + 11,1))
            return false;

    if (!cloudAppendKVHex(
            "RELAY", cloudReportBuffer + relayOffset, statusBytes))
        return false;

    if (!cloudAppendKVHex(
            "TOUCH", cloudReportBuffer + touchOffset, statusBytes))
        return false;

    char countText[4];
    snprintf(countText, sizeof(countText), "%u",
             (unsigned int)timerCount);

    if (!cloudAppendKVText("TIMER_COUNT", countText))
        return false;

    for (uint8_t i = 0; i < timerCount; i++)
    {
        char key[12];
        snprintf(key, sizeof(key), "TIMER%u", (unsigned int)i);

        if (!cloudAppendKVHex(
                key,
                cloudReportBuffer + timerOffset + ((size_t)i * 12U),
                12))
            return false;
    }
      uint8_t ackBytes[TOUCH_COUNT * 4U];

      for (uint8_t i = 0; i < TOUCH_COUNT; i++)
      {
          uint32_t version = cloudCommandGetApplied(i);

          ackBytes[i * 4U]     = (uint8_t)(version);
          ackBytes[i * 4U + 1] = (uint8_t)(version >> 8);
          ackBytes[i * 4U + 2] = (uint8_t)(version >> 16);
          ackBytes[i * 4U + 3] = (uint8_t)(version >> 24);
      }

      if (!cloudAppendKVHex("ACKVER", ackBytes, sizeof(ackBytes)))
          return false;

    if (!cloudAppendKVText("COUNTER", counterText))
        return false;

    if (!cloudAppendKVHex("SIGNATURE", signature, 32))
        return false;

    return true;
}

static bool cloudBuildPushKeyValueRequest(
    const char* counterText,
    uint8_t channel,
    bool state)
{
    if (counterText == nullptr || !cloudDeviceSecretValid)
        return false;

    uint8_t mac[6];
    uint8_t payload[8];
    uint8_t signature[32];

    WiFi.macAddress(mac);

    memcpy(payload, mac, 6);
    payload[6] = channel;
    payload[7] = state ? 1 : 0;

    static const uint8_t domain[] = "DEVICE_TO_SERVER";

    uint8_t signedData[64];
    size_t offset = 0;

    const size_t domainLength = sizeof(domain) - 1U;
    const size_t counterLength = strlen(counterText);

    if (counterLength == 0 || counterLength > 20)
        return false;

    memcpy(signedData + offset, domain, domainLength);
    offset += domainLength;

    signedData[offset++] = 0;

    memcpy(signedData + offset, counterText, counterLength);
    offset += counterLength;

    signedData[offset++] = 0;

    memcpy(signedData + offset, payload, sizeof(payload));
    offset += sizeof(payload);

    bool hmacOk = cloudCalculateHMAC(
        cloudDeviceSecret,
        CLOUD_SECRET_SIZE,
        signedData,
        offset,
        signature);

    memset(signedData, 0, sizeof(signedData));

    if (!hmacOk)
        return false;

    cloudReportTextLength = 0;
    cloudReportTextBuffer[0] = '\0';

    char channelText[4];
    char stateText[2];

    snprintf(channelText, sizeof(channelText), "%u", channel);
    snprintf(stateText, sizeof(stateText), "%u", state ? 1 : 0);

    bool ok =
        cloudAppendKVText("COMMAND", "RELAY") &&
        cloudAppendKVHex("MAC", mac, sizeof(mac)) &&
        cloudAppendKVText("CHANNEL", channelText) &&
        cloudAppendKVText("STATE", stateText) &&
        cloudAppendKVText("COUNTER", counterText) &&
        cloudAppendKVHex("SIGNATURE", signature, sizeof(signature));

    memset(signature, 0, sizeof(signature));
    memset(payload, 0, sizeof(payload));

    return ok;
}

static bool cloudCalculateDeviceRequestHMAC(
    const char* counterText,
    uint8_t signature[32])
{
    if (counterText == nullptr || signature == nullptr)
        return false;

    static const uint8_t domain[] = "DEVICE_TO_SERVER";

    uint8_t data[512];
    size_t offset = 0;

    const size_t domainLength = sizeof(domain) - 1U;
    const size_t counterLength = strlen(counterText);

    if (counterLength == 0 || counterLength > 20)
        return false;

    const size_t required =
        domainLength + 1U + counterLength + 1U + cloudReportLength;

    if (required > sizeof(data))
        return false;

    memcpy(data + offset, domain, domainLength);
    offset += domainLength;

    data[offset++] = 0;

    memcpy(data + offset, counterText, counterLength);
    offset += counterLength;

    data[offset++] = 0;

    memcpy(data + offset, cloudReportBuffer, cloudReportLength);
    offset += cloudReportLength;

    bool ok = cloudCalculateHMAC(
        cloudDeviceSecret,
        CLOUD_SECRET_SIZE,
        data,
        offset,
        signature);

    memset(data, 0, sizeof(data));
    return ok;
}

static bool cloudValidateSignedResponse(
    const String& body,
    const char* expectedCounter,
    String& relayResponse,
    uint32_t commandVersions[TOUCH_COUNT])
{
    if (expectedCounter == nullptr ||
        !cloudDeviceSecretValid ||
        commandVersions == nullptr)
        return false;

    // Exact format:
    // COUNTER=...&RELAY=...&CMDVER=...&SIGNATURE=...

    int p1 = body.indexOf('&');
    if (p1 <= 0)
        return false;

    int p2 = body.indexOf('&', p1 + 1);
    if (p2 <= p1 + 1)
        return false;
    int p3 = body.indexOf('&', p2 + 1);
    if (p3 <= p2 + 1)
        return false;

    if (body.indexOf('&', p3 + 1) >= 0)
        return false;

    String counterField = body.substring(0, p1);
    String relayField = body.substring(p1 + 1, p2);
    String cmdverField = body.substring(p2 + 1, p3);
    String signatureField = body.substring(p3 + 1);

    if (!counterField.startsWith("COUNTER=") ||
        !relayField.startsWith("RELAY=") ||
        !cmdverField.startsWith("CMDVER=") ||
        !signatureField.startsWith("SIGNATURE="))
        return false;

    String receivedCounter = counterField.substring(8);
    relayResponse = relayField.substring(6);
    String cmdverText = cmdverField.substring(7);
    String signatureHex = signatureField.substring(10);

    // COUNTER validation
    if (receivedCounter.length() == 0 ||
        receivedCounter.length() > 20 ||
        receivedCounter != expectedCounter)
        return false;

    for (size_t i = 0; i < receivedCounter.length(); i++)
    {
        if (receivedCounter[i] < '0' ||
            receivedCounter[i] > '9')
            return false;
    }

    if (receivedCounter.length() > 1 &&
        receivedCounter[0] == '0')
        return false;

    // RELAY validation
    if (relayResponse.length() != TOUCH_COUNT)
        return false;

    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        if (relayResponse[i] != '0' &&
            relayResponse[i] != '1')
            return false;

        commandVersions[i] = 0;
    }

    // CMDVER is mandatory.
    // Parse exactly TOUCH_COUNT comma-separated uint32 values.

    int start = 0;

    for (uint8_t i = 0; i < TOUCH_COUNT; i++)
    {
        int comma = cmdverText.indexOf(',', start);

        int end = (i == TOUCH_COUNT - 1)
            ? cmdverText.length()
            : comma;

        if (end < 0 ||
            (i < TOUCH_COUNT - 1 && comma < 0))
            return false;

        if (i == TOUCH_COUNT - 1 && comma >= 0)
            return false;

        String value = cmdverText.substring(start, end);

        if (value.length() == 0 || value.length() > 10)
            return false;

        uint32_t parsed = 0;

        for (size_t j = 0; j < value.length(); j++)
        {
            char c = value[j];

            if (c < '0' || c > '9')
                return false;

            uint8_t digit = (uint8_t)(c - '0');

            if (parsed > (0xFFFFFFFFUL - digit) / 10UL)
                return false;

            parsed = parsed * 10UL + digit;
        }

        if (value.length() > 1 && value[0] == '0')
            return false;

        commandVersions[i] = parsed;

        start = end + 1;
    }

    // SIGNATURE validation
    if (signatureHex.length() != 64)
        return false;

    uint8_t receivedSignature[32];

    for (uint8_t i = 0; i < 32; i++)
    {
        int hi = cloudHexNibble(signatureHex[i * 2]);
        int lo = cloudHexNibble(signatureHex[i * 2 + 1]);

        if (hi < 0 || lo < 0)
            return false;

        receivedSignature[i] =
            (uint8_t)((hi << 4) | lo);
    }

    // Build signed response:
    // SERVER_TO_DEVICE + NUL
    // + COUNTER + NUL
    // + RELAY + NUL
    // + CMDVER

    static const uint8_t domain[] = "SERVER_TO_DEVICE";

    uint8_t signedData[256];
    size_t offset = 0;

    const size_t domainLength = sizeof(domain) - 1U;
    const size_t counterLength = receivedCounter.length();
    const size_t relayLength = relayResponse.length();
    const size_t cmdverLength = cmdverText.length();

    size_t required =
        domainLength + 1U +
        counterLength + 1U +
        relayLength + 1U +
        cmdverLength;

    if (required > sizeof(signedData))
        return false;

    memcpy(signedData + offset, domain, domainLength);
    offset += domainLength;

    signedData[offset++] = 0;

    memcpy(
        signedData + offset,
        receivedCounter.c_str(),
        counterLength
    );
    offset += counterLength;

    signedData[offset++] = 0;
    memcpy(
        signedData + offset,
        relayResponse.c_str(),
        relayLength
    );
    offset += relayLength;

    signedData[offset++] = 0;

    memcpy(
        signedData + offset,
        cmdverText.c_str(),
        cmdverLength
    );
    offset += cmdverLength;

    uint8_t calculatedSignature[32];

    bool hmacOk = cloudCalculateHMAC(
        cloudDeviceSecret,
        CLOUD_SECRET_SIZE,
        signedData,
        offset,
        calculatedSignature
    );

    memset(signedData, 0, sizeof(signedData));

    if (!hmacOk)
        return false;

    uint8_t difference = 0;

    for (uint8_t i = 0; i < 32; i++)
        difference |= receivedSignature[i] ^ calculatedSignature[i];

    memset(receivedSignature, 0, sizeof(receivedSignature));
    memset(calculatedSignature, 0, sizeof(calculatedSignature));

    return difference == 0;
}

/*
  static void cloudDebugDumpTx(const char *method, const char *path,
                              const char *headers, size_t headerLength,
                              const uint8_t *body, size_t bodyLength)
  {
      DBG_PRINTLN("");
      DBG_PRINTLN("========== HTTP TX ==========");
      DBG_PRINT("METHOD: ");
      DBG_PRINTLN(method);
      DBG_PRINT("URL: http://");
      DBG_PRINT(CLOUD_HOST);
      DBG_PRINTLN(path);

      DBG_PRINTLN("--- HTTP HEADERS ---");
      for (size_t i = 0; i < headerLength; i++)
      {
          char ch[2] = {headers[i], '\0'};
          DBG_PRINT(ch);
      }
      DBG_PRINTLN("");

      DBG_PRINT("BODY LENGTH: ");
      DBG_PRINTLN((unsigned int)bodyLength);

      if (body != nullptr && bodyLength > 0)
      {
            DBG_PRINTLN("--- BODY HEX ---");
            cloudDebugPrintHex(body, bodyLength);

          DBG_PRINTLN("--- BODY URL-ENCODED ---");
          cloudDebugPrintUrlEncoded(body, bodyLength);

          // A diagnostic URL representation of the POST packet. This is NOT
          // the actual request URL and is NOT sent to the server as a query.
          DBG_PRINT("--- FULL PACKET URL (diagnostic only): http://");
          DBG_PRINT(CLOUD_HOST);
          DBG_PRINT(path);
          DBG_PRINT(strchr(path, '?') ? "&packet=" : "?packet=");
          cloudDebugPrintUrlEncoded(body, bodyLength);
      }
      else
      {
          DBG_PRINTLN("BODY: <empty>");
      }
      DBG_PRINTLN("=============================");
  }
*/
static void cloudDebugDumpRx()
{
    DBG_PRINTLN("");
    DBG_PRINTLN("========== HTTP RX ==========");
    DBG_PRINT("RESPONSE LENGTH: ");
    DBG_PRINTLN((unsigned int)cloudRxLength);

    if (cloudRxLength == 0)
    {
        DBG_PRINTLN("RESPONSE: <empty>");
        DBG_PRINTLN("=============================");
        return;
    }

    DBG_PRINTLN("--- RAW HTTP RESPONSE ---");
    for (size_t i = 0; i < cloudRxLength; i++)
    {
        const char c = cloudRxBuffer[i];
        if (c == '\r')
            DBG_PRINT("\\r");
        else if (c == '\n')
            DBG_PRINTLN("\\n");
        else if (c >= 32 && c <= 126)
        {
            char ch[2] = {c, '\0'};
            DBG_PRINT(ch);
        }
        else
            DBG_PRINT(".");
    }
    DBG_PRINTLN("");

    //  DBG_PRINTLN("--- FULL RESPONSE HEX ---");
    //  cloudDebugPrintHex((const uint8_t *)cloudRxBuffer, cloudRxLength);

    // DBG_PRINTLN("--- FULL RESPONSE URL-ENCODED ---");
    // cloudDebugPrintUrlEncoded((const uint8_t *)cloudRxBuffer, cloudRxLength);

    DBG_PRINTLN("=============================");
}

//======================================================
// SMALL HTTP RESPONSE PARSER
//======================================================

static bool cloudTextEqualsIgnoreCase(const char* a, const char* b, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        char ca = a[i];
        char cb = b[i];

        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');

        if (ca != cb)
            return false;
    }

    return true;
}

static bool cloudHeaderNameEquals(const char* line, size_t nameLength,
                                  const char* expected)
{
    size_t expectedLength = strlen(expected);
    return nameLength == expectedLength &&
           cloudTextEqualsIgnoreCase(line, expected, nameLength);
}

static bool cloudParseHttpResponse()
{
    char* headerEnd = strstr(cloudRxBuffer, "\r\n\r\n");
    if (headerEnd == nullptr)
        return false;

    // Parse HTTP status code from the first response line.
    char* firstSpace = strchr(cloudRxBuffer, ' ');
    if (firstSpace == nullptr)
        return false;

    cloudResponseCode = atoi(firstSpace + 1);

    // This implementation expects a short response body and a server that
    // closes the connection. Chunked bodies are rejected rather than parsed
    // as if they were a valid relay-status string.
    char* line = strstr(cloudRxBuffer, "\r\n");
    if (line == nullptr)
        return false;

    line += 2;
    while (line < headerEnd)
    {
        char* lineEnd = strstr(line, "\r\n");
        if (lineEnd == nullptr || lineEnd > headerEnd)
            break;

        char* colon = strchr(line, ':');
        if (colon != nullptr && colon < lineEnd)
        {
            size_t nameLength = (size_t)(colon - line);
            if (cloudHeaderNameEquals(line, nameLength, "Transfer-Encoding"))
            {
                const char* value = colon + 1;
                while (value < lineEnd && (*value == ' ' || *value == '\t'))
                    value++;

                if ((size_t)(lineEnd - value) >= 7 &&
                    cloudTextEqualsIgnoreCase(value, "chunked", 7))
                {
                    cloudRequestFailed = true;
                    return false;
                }
            }
        }

        line = lineEnd + 2;
    }

    return true;
}

static bool cloudGetResponseBody(String& body)
{
    char* headerEnd = strstr(cloudRxBuffer, "\r\n\r\n");
    if (headerEnd == nullptr)
        return false;

    const char* bodyStart = headerEnd + 4;
    body = String(bodyStart);
    body.trim();
    return true;
}

//======================================================
// BINARY STATUS PACKET (MAC + IPv4 + DeviceInfo + payload)
//======================================================

static uint8_t cloudEncodeCount(uint8_t count)
{
    if (count == 4) return 0;
    if (count == 8) return 1;
    if (count == 16) return 2;
    return 3; // reserved / unsupported count
}

static uint8_t cloudTimerRuntimeStatus(const Timer &t)
{
    if (!t.enabled)
        return 0;

    if ((t.mode == TIMER_MODE_BLINKER && t.blinkStartTime != 0) ||
        (t.mode != TIMER_MODE_BLINKER && t.activeUntil != 0))
        return TIMER_STATUS_RUNNING;

    if (t.cancelledOccurrenceSecond != 0 &&
        t.cancelledOccurrenceSecond == t.lastTriggerSecond)
        return TIMER_STATUS_CANCELLED;

    return TIMER_STATUS_QUEUED;
}

static void cloudBuildTimerRecord(uint8_t *dst, uint8_t index)
{
    Timer &t = timers[index];
    TimerConfigPacket record;
    memset(&record, 0, sizeof(record));

    const uint8_t relayIndex = t.enabled ? t.relayIndex : (index / 2);
    const uint8_t timerIndex = index % 2;
    record.timerId = (uint8_t)(((relayIndex & 0x0F) << 4) | timerIndex);

    uint8_t timerType = TIMER_PROTOCOL_DISABLED;
    uint8_t runtimeStatus = 0;
    if (t.enabled)
    {
        timerType = (t.mode == TIMER_MODE_BLINKER)
            ? TIMER_PROTOCOL_BLINKER
            : TIMER_PROTOCOL_SCHEDULER;
        runtimeStatus = cloudTimerRuntimeStatus(t);
    }
    record.typeStatus = (uint8_t)((runtimeStatus << 4) | (timerType & 0x0F));

    if (t.enabled)
    {
        if (t.type == TIMER_DAILY)
            record.weekMask = 0x80;
        else if (t.type == TIMER_WEEKLY)
            record.weekMask = t.weekMask;
        else
            record.weekMask = 0; // one-shot; calendar date is not part of the 12-byte wire record

        record.startHour = t.hour;
        record.startMinute = t.minute;

        if (t.mode == TIMER_MODE_BLINKER)
        {
            timerWriteU24LE(record.timeOn, t.blinkOnSeconds);
            timerWriteU24LE(record.timeOff, t.blinkOffSeconds);
            record.count = (uint8_t)t.blinkerCount;
        }
        else
        {
            timerWriteU24LE(record.timeOn, t.durationSeconds);
            timerWriteU24LE(record.timeOff, 0);
            record.count = 0;
        }
    }

    memcpy(dst, &record, sizeof(record));
}

static bool cloudBuildStatusPacket()
{
    const size_t statusBytes = (TOUCH_COUNT + 7U) / 8U;
    const size_t expectedLength =
        12U +
        (2U * statusBytes) +
        1U +
        (MAX_TIMERS * sizeof(TimerConfigPacket)) +
        (TOUCH_COUNT * 4U);

    size_t offset = 0;

    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    memcpy(cloudReportBuffer + offset, mac, sizeof(mac));
    offset += sizeof(mac);

    IPAddress ip = WiFi.localIP();
    for (uint8_t i = 0; i < 4; i++)
        cloudReportBuffer[offset++] = ip[i];

    const uint8_t relayCountCode = cloudEncodeCount(TOUCH_COUNT);
    const uint8_t touchCountCode = cloudEncodeCount(TOUCH_COUNT);
    const uint8_t deviceInfo = (uint8_t)(
        (DEVICE_TYPE_CODE & 0x0F) |
        ((relayCountCode & 0x03) << 4) |
        ((touchCountCode & 0x03) << 6));
    cloudReportBuffer[offset++] = deviceInfo;
    cloudRequestEventFlags = cloudEventFlags;

    cloudReportBuffer[offset++] = cloudRequestEventFlags;

    // Status bitmaps grow from 1 to 2 bytes when the device has 9..16 channels.
    for (size_t byteIndex = 0; byteIndex < statusBytes; byteIndex++)
    {
        uint8_t relayStatus = 0;
        for (uint8_t bit = 0; bit < 8; bit++)
        {
            const uint16_t channel = (uint16_t)(byteIndex * 8U + bit);
            if (channel >= TOUCH_COUNT)
                break;
            if (relayState[channel])
                relayStatus |= (uint8_t)(1U << bit);
        }
        cloudReportBuffer[offset++] = relayStatus;
    }
    for (size_t byteIndex = 0; byteIndex < statusBytes; byteIndex++)
    {
        uint8_t touchStatus = 0;
        for (uint8_t bit = 0; bit < 8; bit++)
        {
            const uint16_t channel = (uint16_t)(byteIndex * 8U + bit);
            if (channel >= TOUCH_COUNT)
                break;
            if (channel < 8 && !bitRead(lastInputs, channel))
                touchStatus |= (uint8_t)(1U << bit);
        }
        cloudReportBuffer[offset++] = touchStatus;
    }
    cloudReportBuffer[offset++] = (uint8_t)MAX_TIMERS;

    for (uint8_t i = 0; i < MAX_TIMERS; i++)
    {
        cloudBuildTimerRecord(cloudReportBuffer + offset, i);
        offset += sizeof(TimerConfigPacket);
    }
            // ACKVER: applied command version for each relay (uint32 LE)
            for (uint8_t i = 0; i < TOUCH_COUNT; i++)
            {
                uint32_t version = cloudCommandGetApplied(i);

                cloudReportBuffer[offset++] = (uint8_t)(version);
                cloudReportBuffer[offset++] = (uint8_t)(version >> 8);
                cloudReportBuffer[offset++] = (uint8_t)(version >> 16);
                cloudReportBuffer[offset++] = (uint8_t)(version >> 24);
            }
    cloudReportLength = offset;
    return cloudReportLength == expectedLength;
}

//======================================================
// ASYNC TCP CALLBACKS
// Callbacks only collect network data / flags. Relay logic is handled later
// by taskCloudSync() in the normal main loop.
//======================================================

static void cloudOnConnect(void* arg, AsyncClient* client)
{
    (void)arg;

    char request[320];

    int n = snprintf(
        request,
        sizeof(request),
        "POST %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "Accept: text/plain\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: %u\r\n"
        "\r\n",
        cloudRequestPath,
        CLOUD_HOST,
        (unsigned int)cloudReportTextLength
    );

    bool writeFailed = (n <= 0 || (size_t)n >= sizeof(request));

    if (!writeFailed)
    {
        size_t headerWritten = client->write(request, (size_t)n);
        size_t bodyWritten = 0;

        if (headerWritten > 0)
        {
            bodyWritten = client->write(
                cloudReportTextBuffer,
                cloudReportTextLength);
        }

        writeFailed = (headerWritten == 0 || bodyWritten == 0);
    }

    if (writeFailed)
    {
        cloudRequestFailed = true;
        client->close(true);
    }
}

static void cloudOnData(void* arg, AsyncClient* client, void* data, size_t len)
{
    (void)arg;

    if (cloudRequestFailed || len == 0)
        return;

    if (cloudRxLength + len >= CLOUD_RX_BUFFER_SIZE)
    {
        cloudRequestFailed = true;
        client->close(true);
        return;
    }

    memcpy(cloudRxBuffer + cloudRxLength, data, len);
    cloudRxLength += len;
    cloudRxBuffer[cloudRxLength] = '\0';
}

static void cloudOnDisconnect(void* arg, AsyncClient* client)
{
    (void)arg;
    (void)client;

    cloudRequestInFlight = false;
    cloudRequestDone = true;
}

static void cloudOnError(void* arg, AsyncClient* client, int8_t error)
{
    (void)arg;
    (void)error;

    cloudRequestFailed = true;
    client->close(true);
}

//======================================================
// START A REQUEST (NON-BLOCKING)
//======================================================

static bool cloudStartRequest(
    CloudRequestType type,
    const char* path,
    byte channel = 0,
    bool state = false)
{
    if (cloudRequestInFlight || path == nullptr)
        return false;

    size_t pathLength = strlen(path);

    if (pathLength == 0 ||
        pathLength >= sizeof(cloudRequestPath))
        return false;

    strcpy(cloudRequestPath, path);

    if (type != CLOUD_REQ_POLL &&
        type != CLOUD_REQ_PUSH)
        return false;

    if (!cloudDeviceSecretValid)
    {
        DBG_PRINTLN("[Crypto] Cannot send request: Secret not provisioned");
        return false;
    }

    uint64_t counterValue;

    if (!cloudGetNextCounter(counterValue))
    {
        DBG_PRINTLN("[Crypto] Cannot get next Counter");
        return false;
    }

    cloudUint64ToDecimal(
        counterValue,
        cloudRequestCounterText);

    bool buildOk = false;

    if (type == CLOUD_REQ_POLL)
    {
        if (!cloudBuildStatusPacket())
            return false;

        uint8_t hmac[32];

        if (!cloudCalculateDeviceRequestHMAC(
                cloudRequestCounterText,
                hmac))
        {
            memset(hmac, 0, sizeof(hmac));
            return false;
        }

        buildOk = cloudBuildKeyValueReport(
            cloudRequestCounterText,
            hmac);

        memset(hmac, 0, sizeof(hmac));
    }
    else
    {
        buildOk = cloudBuildPushKeyValueRequest(
            cloudRequestCounterText,
            channel,
            state);
    }

    if (!buildOk)
    {
        DBG_PRINTLN("[Cloud] Request build FAILED");
        return false;
    }

    DBG_PRINT("[Cloud] Counter: ");
    DBG_PRINTLN(cloudRequestCounterText);

    DBG_PRINT("[Cloud] POST length: ");
    DBG_PRINTLN((unsigned int)cloudReportTextLength);

    memset(cloudRxBuffer, 0, sizeof(cloudRxBuffer));
    cloudRxLength = 0;
    cloudResponseCode = 0;

    cloudRequestType = type;
    cloudRequestChannel = channel;
    cloudRequestRelayState = state;
    cloudRequestFailed = false;
    cloudRequestDone = false;
    cloudRequestInFlight = true;
    cloudRequestStartedAt = millis();

    if (!cloudClient.connect(CLOUD_HOST, CLOUD_PORT))
    {
        cloudRequestInFlight = false;
        cloudRequestDone = true;
        cloudRequestFailed = true;
        return false;
    }

    return true;
}

//======================================================
// LOCAL CHANGE TRACKING
//======================================================

static void cloudTrackLocalRelayChanges()
{
    if (!cloudObservedInitialized)
    {
        for (byte i = 0; i < TOUCH_COUNT; i++)
            cloudObservedRelayState[i] = relayState[i];

        cloudObservedInitialized = true;
        return;
    }

    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        if (relayState[i] != cloudObservedRelayState[i])
        {
            cloudObservedRelayState[i] = relayState[i];
            cloudLocalPending[i] = true;
        }
    }
}

//======================================================
// PROCESS COMPLETED REQUESTS IN THE MAIN LOOP
//======================================================

static void cloudIncreaseRetry(unsigned long& retryMs)
{
    if (retryMs < CLOUD_RETRY_MIN_MS)
        retryMs = CLOUD_RETRY_MIN_MS;
    else if (retryMs < CLOUD_RETRY_MAX_MS)
    {
        retryMs *= 2;
        if (retryMs > CLOUD_RETRY_MAX_MS)
            retryMs = CLOUD_RETRY_MAX_MS;
    }
}

static void cloudProcessCompletedRequest()
{
    if (!cloudRequestDone)
        return;
				
								  
		 

    cloudRequestDone = false;

    // Print the complete response from the main loop (not inside the async
    // receive callback) so the callback can keep collecting bytes quickly.
     cloudDebugDumpRx();
//
bool responseParsed = false;

if (!cloudRequestFailed)
    responseParsed = cloudParseHttpResponse();

if (responseParsed && cloudResponseCode == 409)
{
    String errorBody;

    if (cloudGetResponseBody(errorBody))
    {
        if (cloudRecoverCounterFromResponse(errorBody))
        {
            DBG_PRINTLN(
                "[Crypto] Server Counter mismatch recovered"
            );
        }
        else
        {
            DBG_PRINTLN(
                "[Crypto] Counter recovery failed"
            );
        }
    }
}

bool requestOk =
    responseParsed &&
    cloudResponseCode >= 200 &&
    cloudResponseCode < 300;
//
    if (cloudRequestType == CLOUD_REQ_POLL)
    {
        if (requestOk && cloudResponseCode == 200)
        {
            String response;
            String relayResponse;

            uint32_t commandVersions[TOUCH_COUNT] = {0};

            requestOk = cloudGetResponseBody(response);

            if (requestOk)
            {
                requestOk = cloudValidateSignedResponse(
                    response,
                    cloudRequestCounterText,
                    relayResponse,
                    commandVersions);
            }

            if (requestOk)
                response = relayResponse;

            if (requestOk)
            {
                DBG_PRINT("[Cloud] Server status: ");
                DBG_PRINTLN(response);

												  
 
                for (byte i = 0; i < TOUCH_COUNT; i++)
                {
                    bool remoteState = (response[i] == '1');
                    cloudKnownRelayState[i] = remoteState;

                    // Versioned server command has priority over boot baseline
                    // and local-pending reconciliation because it arrived later.
                    if (commandVersions[i] != 0)
                    {
                        uint32_t incomingVersion = commandVersions[i];

                        uint32_t appliedVersion =
                            cloudCommandGetApplied(i);

                        if (incomingVersion <= appliedVersion)
                        {
                            // Already applied or stale command.
                            cloudObservedRelayState[i] = relayState[i];
                            continue;
                        }

                        uint32_t pendingVersion = 0;
                        bool pendingState = false;

                        bool hasPending = cloudCommandGetPending(
                            i,
                            pendingVersion,
                            pendingState);

                        if (hasPending)
                        {
                            // Never execute an older command over a newer pending one.
                            if (pendingVersion > incomingVersion)
                            {
                                cloudObservedRelayState[i] = relayState[i];
                                continue;
                            }

                            // Same version means a previous boot was interrupted.
                            // The stored state must match the server command.
                            if (pendingVersion == incomingVersion &&
                                pendingState != remoteState)
                            {
                                cloudObservedRelayState[i] = relayState[i];
                                continue;
                            }
                        }
                // Persist command before executing it.
                        if (!hasPending || pendingVersion != incomingVersion)
                        {
                            if (!cloudCommandBegin(
                                    i,
                                    incomingVersion,
                                    remoteState))
                            {
                                DBG_PRINT("[Cloud] Journal BEGIN failed, relay=");
                                DBG_PRINTLN(i);

                                cloudObservedRelayState[i] = relayState[i];
                                continue;
                            }
                        }

                        // Always call manual command, even for same-state commands.
                        // This cancels Timer/Blinker for this relay.
                        if (remoteState)
                            manualRelayOn(i);
                        else
                            manualRelayOff(i);

                        // ACK becomes visible only after successful persistent commit.
                        if (!cloudCommandCommit(i))
                        {
                            DBG_PRINT("[Cloud] Journal COMMIT failed, relay=");
                            DBG_PRINTLN(i);

                            cloudObservedRelayState[i] = relayState[i];
                            continue;
                        }

                        cloudKnownRelayState[i] = remoteState;
                        cloudLocalPending[i] = false;
                        cloudObservedRelayState[i] = relayState[i];

                        DBG_PRINT("[Cloud] CMDVER applied relay ");
                        DBG_PRINT(i);
                        DBG_PRINT(" version ");
                        DBG_PRINTLN(incomingVersion);

                        continue;
                    }


                    if (cloudBootBaselinePending)
                    {
                        // Never restore ordinary relay state from the server
                        // after reboot. Keep the locally resolved boot state
                        // (OFF, except valid timer catch-up) and push it up.
                        cloudLocalPending[i] = (relayState[i] != remoteState);
                        cloudObservedRelayState[i] = relayState[i];
                        continue;
                    }

                    // Preserve a local change while cloud sync was unavailable.
                    if (cloudLocalPending[i])
                        continue;

                    if (relayState[i] != remoteState)
                    {
                        if (remoteState)
                            manualRelayOn(i);
                        else
                            manualRelayOff(i);
                    }

                    cloudObservedRelayState[i] = relayState[i];
                }


                    // پاک کردن رخدادهایی که در بسته تأییدشده ارسال شده‌اند
                    cloudClearEventFlags(cloudRequestEventFlags);
                    cloudRequestEventFlags = 0;

                    cloudBootBaselinePending = false;
                    cloudSynced = true;
                    cloudPollRetryMs = CLOUD_POLL_INTERVAL_MS;
                    cloudPushRetryMs = CLOUD_RETRY_MIN_MS;
            }
        }

        if (!requestOk)
        {
            // A failed poll means Internet/server reachability is no longer
            // confirmed. Time manager will stop SNTP on its next pass.
            cloudSynced = false;
            cloudMarkEventFlag(DEVICE_INFO_2_POLL_FAILURE);
            cloudIncreaseRetry(cloudPollRetryMs);
            DBG_PRINT("[Cloud] Poll failed; retry delay ms=");
						   
						   
            DBG_PRINTLN(cloudPollRetryMs);
        }
    }
        else if (cloudRequestType == CLOUD_REQ_PUSH)
        {
            if (requestOk && cloudResponseCode == 200)
            {
                String response;
                String relayResponse;

                requestOk = cloudGetResponseBody(response);

                if (requestOk)
                {
                    uint32_t ignoredCommandVersions[TOUCH_COUNT] = {0};

                    requestOk = cloudValidateSignedResponse(
                        response,
                        cloudRequestCounterText,
                        relayResponse,
                        ignoredCommandVersions);
                }
            }

        if (requestOk)
        {
            byte channel = cloudRequestChannel;
            bool sentState = cloudRequestRelayState;

            cloudKnownRelayState[channel] = sentState;
					  

            if (relayState[channel] == sentState)
                cloudLocalPending[channel] = false;
            else
                cloudLocalPending[channel] = true;

            cloudObservedRelayState[channel] = relayState[channel];
            cloudPushRetryMs = CLOUD_RETRY_MIN_MS;

            DBG_PRINT("[Cloud] Relay ");
            DBG_PRINT(channel);
            DBG_PRINT(" set ");
            DBG_PRINTLN(sentState ? "ON" : "OFF");
        }
        else
        {
            cloudIncreaseRetry(cloudPushRetryMs);
            DBG_PRINT("[Cloud] Relay push failed; retry delay ms=");
            DBG_PRINTLN(cloudPushRetryMs);
        }
    }

    cloudRequestType = CLOUD_REQ_NONE;
    cloudRequestFailed = false;
}

//======================================================
// INIT
						
//======================================================

void initCloudAPI()
{
    cloudLoadEventFlags();
    if (!cloudCommandJournalInit())
    {
        DBG_PRINTLN("[Cloud] Command Journal initialization FAILED");
    }
    if (cloudLoadDeviceSecret())
    {
        DBG_PRINTLN("[Crypto] Device Secret: LOADED");
    }
    else
    {
        DBG_PRINTLN("[Crypto] Device Secret: NOT PROVISIONED");
    }

    cloudInitSequenceCounter();

    if (cloudHMACTest())
       {DBG_PRINTLN("[Crypto] HMAC-SHA256 TEST: PASS");}
    else
        {DBG_PRINTLN("[Crypto] HMAC-SHA256 TEST: FAIL");}

    cloudRequestType = CLOUD_REQ_NONE;
    cloudRequestInFlight = false;
    cloudRequestDone = false;
    cloudRequestFailed = false;

    cloudLastPollAttempt = 0;
    cloudLastPushAttempt = 0;
    cloudPollRetryMs = CLOUD_RETRY_MIN_MS;
    cloudPushRetryMs = CLOUD_RETRY_MIN_MS;

    cloudFirstPoll = true;
    cloudSynced = false;
    cloudBootBaselinePending = true;
    cloudObservedInitialized = false;

    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        cloudKnownRelayState[i] = false;
        cloudObservedRelayState[i] = false;
        cloudLocalPending[i] = false;
    }

    cloudClient.onConnect(cloudOnConnect, nullptr);
    cloudClient.onData(cloudOnData, nullptr);
    cloudClient.onDisconnect(cloudOnDisconnect, nullptr);
    cloudClient.onError(cloudOnError, nullptr);

    DBG_PRINTLN("[Cloud] Async HTTP API initialized (Key=Value POST)");
}

// True only after a valid HTTP poll response has been received.
// Wi-Fi connection by itself does not prove Internet/DNS availability.
bool isCloudServerReachableForTimeSync()
{
     return cloudSynced && (WiFi.status() == WL_CONNECTED);
}

//======================================================
// MAIN CLOUD TASK
// Call from runSystem() on every loop iteration.
//======================================================

void taskCloudSync()
{

   cloudProcessSecretSerial();
    // Detect local state changes regardless of Wi-Fi/server availability.
    cloudTrackLocalRelayChanges();

    // A request that never completes must not remain active forever.
    if (cloudRequestInFlight &&
        (unsigned long)(millis() - cloudRequestStartedAt) >=
        CLOUD_REQUEST_TIMEOUT_MS)
    {
        cloudRequestFailed = true;
        cloudClient.close(true);
    }

    // Network callbacks never change relay state; apply results here.
    cloudProcessCompletedRequest();

    if (cloudRequestInFlight || WiFi.status() != WL_CONNECTED)
        return;

    unsigned long now = millis();

    // Poll immediately at boot; after failures use exponential backoff.
    unsigned long pollInterval = cloudFirstPoll
        ? CLOUD_RETRY_MIN_MS
        : (cloudSynced ? CLOUD_POLL_INTERVAL_MS : cloudPollRetryMs);

    if (cloudFirstPoll ||
        (unsigned long)(now - cloudLastPollAttempt) >= pollInterval)
							   
    {
        cloudLastPollAttempt = now;
        cloudFirstPoll = false;

        cloudStartRequest(CLOUD_REQ_POLL, CLOUD_STATUS_PATH);
	 

													   
													
					 
        return;
    }

    // Send one pending relay state per request. A later local change is
    // represented by the latest relayState[] value, not an unbounded queue.
    if (!cloudSynced ||
        (unsigned long)(now - cloudLastPushAttempt) < cloudPushRetryMs)
								
        return;

														
    for (byte i = 0; i < TOUCH_COUNT; i++)
    {
        if (cloudLocalPending[i] ||
            relayState[i] != cloudKnownRelayState[i])
        {
            cloudLastPushAttempt = now;

               cloudStartRequest(
                CLOUD_REQ_PUSH,
                CLOUD_STATUS_PATH,
                i,
                relayState[i]);
            break;
        }
    }
}
