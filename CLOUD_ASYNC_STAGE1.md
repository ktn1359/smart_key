# Cloud API asynchronous transport — Stage 1

This branch changes `CloudAPI_1.ino` from synchronous `HTTPClient` requests to an asynchronous TCP client.

## Required Arduino libraries

Install the TCP library matching the selected board:

- **ESP-12F / ESP8266:** `ESPAsyncTCP` from the ESP32Async project.
- **ESP32-C3:** `AsyncTCP` from the ESP32Async project.

These are the transport libraries; this firmware implements the small HTTP/1.0 request/response layer itself. It does not use the GPLv3 `AsyncHTTPRequest_Generic` library.

## Current protocol in this stage

- GET `http://helpschool.ir/status/` reads the relay state.
- GET `/seton/?ch=N` and `/setoff/?ch=N` still write relay state.
- HTTP requests are started asynchronously; the main loop does not call blocking `HTTPClient.GET()`.
- The client sends HTTP/1.0 with `Connection: close` and expects a short plain-text response.
- Chunked transfer-encoded responses are rejected. The status endpoint should return a simple short body such as `0100` and close the connection.

## Behavior and known limits

- Local relay/touch/rule/timer processing continues while the cloud request is in progress.
- Local relay changes observed during a server outage are retained as pending states. Once a valid server poll succeeds, pending local states are pushed instead of being overwritten by stale server state.
- Only the latest state per relay is retained; this is state synchronization, not an event-history queue.
- Relay writes still use GET in Stage 1. Stage 2 will migrate mutations to POST after Stage 1 is compiled and tested.
- The code has not been compiled against the exact Arduino core and library versions used by the project yet. Compile and hardware tests are required before merging this branch into `main`.

## Suggested test sequence

1. Compile for ESP8266 with `ESPAsyncTCP` installed.
2. Boot with the server available; confirm the initial relay status synchronizes.
3. Disconnect the server while keeping Wi-Fi connected; test touch, physical/local control, rules, and timers.
4. Change relays during the outage; confirm the device remains responsive.
5. Restore the server; confirm local pending states are sent and stale server status does not undo those local changes.
6. Repeat with Wi-Fi disconnected/reconnected.
7. Compile for ESP32-C3 with `AsyncTCP` installed and repeat the same tests.
