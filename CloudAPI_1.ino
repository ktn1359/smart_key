//======================================================
//        CLOUD / CUSTOM SERVER SYNC (asynchronous HTTP)
//======================================================


																	
																				  
															   
																								   
																									 
																														  
//
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


//======================================================
// SERVER
//======================================================

static const char* CLOUD_HOST = "helpschool.ir";
static const uint16_t CLOUD_PORT = 80;
static const char* CLOUD_STATUS_PATH = "/kp4000/test.php";

static const unsigned long CLOUD_POLL_INTERVAL_MS = 10000UL;
static const unsigned long CLOUD_REQUEST_TIMEOUT_MS = 6000UL;
static const unsigned long CLOUD_RETRY_MIN_MS = 1000UL;
static const unsigned long CLOUD_RETRY_MAX_MS = 30000UL;
static const size_t CLOUD_RX_BUFFER_SIZE = 768;
static const size_t CLOUD_REPORT_MAX_SIZE = 11U + 5U + (MAX_TIMERS * 12U);
static uint8_t cloudReportBuffer[CLOUD_REPORT_MAX_SIZE];
static size_t cloudReportLength = 0;

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

     DBG_PRINTLN("--- FULL RESPONSE HEX ---");
     cloudDebugPrintHex((const uint8_t *)cloudRxBuffer, cloudRxLength);

    DBG_PRINTLN("--- FULL RESPONSE URL-ENCODED ---");
    cloudDebugPrintUrlEncoded((const uint8_t *)cloudRxBuffer, cloudRxLength);

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
    const size_t expectedLength = 11U + (2U * statusBytes) + 1U +
                                  (MAX_TIMERS * sizeof(TimerConfigPacket));
    if (expectedLength > sizeof(cloudReportBuffer))
        return false;

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
    int n = 0;

    if (cloudRequestType == CLOUD_REQ_POLL)
    {
        n = snprintf(
            request,
            sizeof(request),
            "POST %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "Accept: text/plain\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: %u\r\n"
            "\r\n",
            cloudRequestPath,
            CLOUD_HOST,
            (unsigned int)cloudReportLength
        );

        bool writeFailed = (n <= 0 || (size_t)n >= sizeof(request));
        if (!writeFailed)
        {
            size_t headerWritten = client->write(request, (size_t)n);
            size_t bodyWritten = 0;
            if (headerWritten > 0)
                bodyWritten = client->write((const char *)cloudReportBuffer,
                                            cloudReportLength);

            // Dump only after the actual writes, so serial output does not
            // delay the start of the POST transmission.
            cloudDebugDumpTx("POST", cloudRequestPath, request, (size_t)n,
                             cloudReportBuffer, cloudReportLength);

            writeFailed = (headerWritten == 0 || bodyWritten == 0);
        }

        if (writeFailed)
        {
            cloudRequestFailed = true;
            client->close(true);
        }
    }
    else
    {
        n = snprintf(
            request,
            sizeof(request),
            "GET %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "Accept: text/plain\r\n"
            "\r\n",
            cloudRequestPath,
            CLOUD_HOST
        );

        bool writeFailed = (n <= 0 || (size_t)n >= sizeof(request));
        if (!writeFailed)
        {
            size_t headerWritten = client->write(request, (size_t)n);
            // Dump after the write to avoid delaying command transmission.
            cloudDebugDumpTx("GET", cloudRequestPath, request, (size_t)n,
                             nullptr, 0);
            writeFailed = (headerWritten == 0);
        }

        if (writeFailed)
        {
            cloudRequestFailed = true;
            client->close(true);
        }
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

static bool cloudStartRequest(CloudRequestType type, const char* path,
                              byte channel = 0, bool state = false)
{
    if (cloudRequestInFlight || path == nullptr)
        return false;

    size_t pathLength = strlen(path);
    if (pathLength == 0 || pathLength >= sizeof(cloudRequestPath))
        return false;

    strcpy(cloudRequestPath, path);

    if (type == CLOUD_REQ_POLL && !cloudBuildStatusPacket())
        return false;

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

    bool requestOk = !cloudRequestFailed;
    if (requestOk)
    {
        if (!cloudParseHttpResponse())
            requestOk = false;
        else if (cloudResponseCode < 200 || cloudResponseCode >= 300)
            requestOk = false;
    }

    if (cloudRequestType == CLOUD_REQ_POLL)
    {
        if (requestOk && cloudResponseCode == 200)
        {
            String response;
            requestOk = cloudGetResponseBody(response);

            if (requestOk && response.length() == TOUCH_COUNT)
            {
                for (byte i = 0; i < TOUCH_COUNT; i++)
                {
                    if (response[i] != '0' && response[i] != '1')
                    {
                        requestOk = false;
                        break;
                    }
                }
            }
            else
            {
                requestOk = false;
            }

            if (requestOk)
            {
                DBG_PRINT("[Cloud] Server status: ");
                DBG_PRINTLN(response);

												  
 
                for (byte i = 0; i < TOUCH_COUNT; i++)
                {
                    bool remoteState = (response[i] == '1');
                    cloudKnownRelayState[i] = remoteState;

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
            cloudIncreaseRetry(cloudPollRetryMs);
            DBG_PRINT("[Cloud] Poll failed; retry delay ms=");
						   
						   
            DBG_PRINTLN(cloudPollRetryMs);
        }
    }
    else if (cloudRequestType == CLOUD_REQ_PUSH)
    {
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

    DBG_PRINTLN("[Cloud] Async HTTP API initialized (binary status POST)");
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
            char path[48];
            snprintf(
                path,
                sizeof(path),
                relayState[i] ? "/seton/?ch=%u" : "/setoff/?ch=%u",
                (unsigned int)i
            );

            cloudLastPushAttempt = now;
            cloudStartRequest(CLOUD_REQ_PUSH, path, i, relayState[i]);
            break;
        }
    }
}
