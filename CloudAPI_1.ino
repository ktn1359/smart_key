//======================================================
//        CLOUD / CUSTOM SERVER SYNC (asynchronous HTTP)
//======================================================
//
// Stage 1:
// - Async TCP transport: no HTTPClient/GET() wait in runSystem().
// - Local relay/touch/rule/timer logic continues while server is unreachable.
// - Local relay changes made during an outage are kept and pushed after reconnect.
// - GET endpoints are intentionally retained in this stage.
//   Stage 2 will migrate relay mutations to POST after the transport is tested.
//
// Dependencies:
//   ESP8266: ESPAsyncTCP (ESP32Async organization)
//   ESP32-C3: AsyncTCP (ESP32Async organization)
//
// The server must return a short plain-text status body from /status/,
// e.g. "0100". This client sends HTTP/1.0 with Connection: close.

#if MODULE_TYPE == MODULE_ESP12F
    #include <ESPAsyncTCP.h>
#elif MODULE_TYPE == MODULE_ESP32C3
    #include <AsyncTCP.h>
#endif

//======================================================
// SERVER
//======================================================

static const char* CLOUD_HOST = "helpschool.ir";
static const uint16_t CLOUD_PORT = 80;
static const char* CLOUD_STATUS_PATH = "/status/";

static const unsigned long CLOUD_POLL_INTERVAL_MS = 10000UL;
static const unsigned long CLOUD_REQUEST_TIMEOUT_MS = 6000UL;
static const unsigned long CLOUD_RETRY_MIN_MS = 1000UL;
static const unsigned long CLOUD_RETRY_MAX_MS = 30000UL;
static const size_t CLOUD_RX_BUFFER_SIZE = 768;

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
// ASYNC TCP CALLBACKS
// Callbacks only collect network data / flags. Relay logic is handled later
// by taskCloudSync() in the normal main loop.
//======================================================

static void cloudOnConnect(void* arg, AsyncClient* client)
{
    (void)arg;

    char request[256];

    int n = snprintf(
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

    if (n <= 0 || (size_t)n >= sizeof(request) ||
        client->write(request, (size_t)n) == 0)
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

static bool cloudStartRequest(CloudRequestType type, const char* path,
                              byte channel = 0, bool state = false)
{
    if (cloudRequestInFlight || path == nullptr)
        return false;

    size_t pathLength = strlen(path);
    if (pathLength == 0 || pathLength >= sizeof(cloudRequestPath))
        return false;

    strcpy(cloudRequestPath, path);

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

                    // If a local change occurred while cloud sync was
                    // unavailable, preserve it and queue it for upload.
                    if (cloudLocalPending[i])
                    {
                        cloudKnownRelayState[i] = remoteState;
                        continue;
                    }

                    if (relayState[i] != remoteState)
                    {
                        if (remoteState)
                            manualRelayOn(i);
                        else
                            manualRelayOff(i);
                    }

                    cloudObservedRelayState[i] = relayState[i];
                    cloudKnownRelayState[i] = remoteState;
                }

                cloudSynced = true;
                cloudPollRetryMs = CLOUD_POLL_INTERVAL_MS;
                cloudPushRetryMs = CLOUD_RETRY_MIN_MS;
            }
        }

        if (!requestOk)
        {
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

    DBG_PRINTLN("[Cloud] Async HTTP API initialized");
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
