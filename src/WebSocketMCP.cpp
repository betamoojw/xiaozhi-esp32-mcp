#include "WebSocketMCP.h"

#include <Arduino.h>
#include <Client.h>
#include <Preferences.h>  // Required for NVS token persistence
#include "esp_log.h"
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>

// Log tag for ESP-IDF logging system
static const char* TAG = "MCP_WSS";

// === Static constants ===
const int WebSocketMCP::INITIAL_BACKOFF = 1000;      // 1 second
const int WebSocketMCP::MAX_BACKOFF = 60000;         // 60 seconds
const int WebSocketMCP::PING_INTERVAL = 10000;       // 10 seconds
const int WebSocketMCP::DISCONNECT_TIMEOUT = 60000;  // 60 seconds

// === Embedded XiaoZhi Root CA Certificate ===
// Issuer: AAA Certificate Services (Sectigo/Comodo chain)
// Valid for api.xiaozhi.me (WoTrus DV Server CA)
const char XIAOZHI_ROOT_CA[] PROGMEM = \
"-----BEGIN CERTIFICATE-----\n" \
"MIIEMjCCAxqgAwIBAgIBATANBgkqhkiG9w0BAQUFADB7MQswCQYDVQQGEwJHQjEb\n" \
"MBkGA1UECAwSR3JlYXRlciBNYW5jaGVzdGVyMRAwDgYDVQQHDAdTYWxmb3JkMRow\n" \
"GAYDVQQKDBFDb21vZG8gQ0EgTGltaXRlZDEhMB8GA1UEAwwYQUFBIENlcnRpZmlj\n" \
"YXRlIFNlcnZpY2VzMB4XDTA0MDEwMTAwMDAwMFoXDTI4MTIzMTIzNTk1OVowezEL\n" \
"MAkGA1UEBhMCR0IxGzAZBgNVBAgMEkdyZWF0ZXIgTWFuY2hlc3RlcjEQMA4GA1UE\n" \
"BwwHU2FsZm9yZDEaMBgGA1UECgwRQ29tb2RvIENBIExpbWl0ZWQxITAfBgNVBAMM\n" \
"GEFBQSBDZXJ0aWZpY2F0ZSBTZXJ2aWNlczCCASIwDQYJKoZIhvcNAQEBBQADggEP\n" \
"ADCCAQoCggEBAL5AnfRu4ep2hxxNRUSOvkbIgwadwSr+GB+O5AL686tdUIoWMQua\n" \
"BtDFcCLNSS1UY8y2bmhGC1Pqy0wkwLxyTurxFa70VJoSCsN6sjNg4tqJVfMiWPPe\n" \
"3M/vg4aijJRPn2jymJBGhCfHdr/jzDUsi14HZGWCwEiwqJH5YZ92IFCokcdmtet4\n" \
"YgNW8IoaE+oxox6gmf049vYnMlhvB/VruPsUK6+3qszWY19zjNoFmag4qMsXeDZR\n" \
"rOme9Hg6jc8P2ULimAyrL58OAd7vn5lJ8S3frHRNG5i1R8XlKdH5kBjHYpy+g8cm\n" \
"ez6KJcfA3Z3mNWgQIJ2P2N7Sw4ScDV7oL8kCAwEAAaOBwDCBvTAdBgNVHQ4EFgQU\n" \
"oBEKIz6W8Qfs4q8p74Klf9AwpLQwDgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQF\n" \
"MAMBAf8wewYDVR0fBHQwcjA4oDagNIYyaHR0cDovL2NybC5jb21vZG9jYS5jb20v\n" \
"QUFBQ2VydGlmaWNhdGVTZXJ2aWNlcy5jcmwwNqA0oDKGMGh0dHA6Ly9jcmwuY29t\n" \
"b2RvLm5ldC9BQUFDZXJ0aWZpY2F0ZVNlcnZpY2VzLmNybDANBgkqhkiG9w0BAQUF\n" \
"AAOCAQEACFb8AvCb6P+k+tZ7xkSAzk/ExfYAWMymtrwUSWgEdujm7l3sAg9g1o1Q\n" \
"GE8mTgHj5rCl7r+8dFRBv/38ErjHT1r0iWAFf2C3BUrz9vHCv8S5dIa2LX1rzNLz\n" \
"Rt0vxuBqw8M0Ayx9lt1awg6nCpnBBYurDC/zXDrPbDdVCYfeU0BsWO/8tqtlbgT2\n" \
"G9w84FoVxp7Z8VlIMCFlA2zs6SFz7JsDoeA3raAVGI/6ugLOpyypEBMs1OUIJqsi\n" \
"l2D4kF501KKaU73yqWjgom7C12yxow+ev+to51byrvLjKzg6CYG1a4XXvi3tPxq3\n" \
"smPi9WIsgtRqAEFQ8TmDn5XpNpaYbg==\n" \
"-----END CERTIFICATE-----";

// === ToolResponse implementation ===
ToolResponse::ToolResponse(bool err, const char* msg)
    : _error(err), _message(msg ? msg : "") {}

// === WebSocketMCP implementation ===

WebSocketMCP::WebSocketMCP(Client& client)
    : _injectedClient(&client),
      _currentState(WS_DISCONNECTED),
      _port(0),
      _isSecure(false),
      _connectionCallback(nullptr),
      _connected(false),
      _lastReconnectAttempt(0),
      _currentBackoff(INITIAL_BACKOFF),
      _reconnectAttempt(0),
      _lastPingTime(0),
      _awaitingActivation(false)
{
    // Initialize member buffers to empty state
    _host[0] = '\0';
    _path[0] = '\0';
    _receiveBuffer[0] = '\0';
    _pendingAgentCode[0] = '\0';

    ESP_LOGI(TAG, "WebSocketMCP initialized with injected client");
}

// === NVS Helpers (private) ===

String WebSocketMCP::loadTokenFromNVS() {
    Preferences prefs;
    prefs.begin("xiaozhi", true); // read-only
    String token = prefs.getString("mcp_token", "");
    prefs.end();
    if (!token.isEmpty()) {
        ESP_LOGI(TAG, "Loaded JWT token from NVS (length: %u)", token.length());
    }
    return token;
}

bool WebSocketMCP::saveTokenToNVS(const char* token) {
    if (!token || strlen(token) < 50) {
        ESP_LOGE(TAG, "Refusing to save invalid token (length: %u)", token ? strlen(token) : 0);
        return false;
    }

    Preferences prefs;
    prefs.begin("xiaozhi", false); // read-write
    bool success = prefs.putString("mcp_token", token);
    prefs.end();

    if (success) {
        ESP_LOGI(TAG, "Saved JWT token to NVS (length: %u)", strlen(token));
    } else {
        ESP_LOGE(TAG, "Failed to save token to NVS");
    }
    return success;
}

bool WebSocketMCP::isActivated() {
    return !loadTokenFromNVS().isEmpty();
}

void WebSocketMCP::clearActivation() {
    Preferences prefs;
    prefs.begin("xiaozhi", false);
    prefs.clear();
    prefs.end();
    ESP_LOGI(TAG, "Cleared activation (NVS wiped)");
}

// === Activation Flow: beginWithAgentCode ===

bool WebSocketMCP::beginWithAgentCode(const char* agentCode, ConnectionCallback connCb) {
    if (!agentCode || strlen(agentCode) == 0) {
        ESP_LOGE(TAG, "Invalid agent code (null or empty)");
        return false;
    }

    // Store agent code for later use in initialize message
    size_t len = std::min(strlen(agentCode), sizeof(_pendingAgentCode) - 1);
    strncpy(_pendingAgentCode, agentCode, len);
    _pendingAgentCode[len] = '\0';

    _connectionCallback = connCb;
    _awaitingActivation = true;

    // Connect to base endpoint (no token)
    if (!begin("wss://api.xiaozhi.me/mcp", connCb)) {
        ESP_LOGE(TAG, "Failed to parse base endpoint for activation");
        return false;
    }

    ESP_LOGI(TAG, "Activation initiated with agent code: %s", _pendingAgentCode);
    return true;
}

// === Core: begin(endpoint) ===

bool WebSocketMCP::begin(const char* mcpEndpoint, ConnectionCallback connCb) {
    _connectionCallback = connCb;

    // Case 1: nullptr → load token from NVS and use default endpoint
    if (mcpEndpoint == nullptr) {
        String token = loadTokenFromNVS();
        if (token.isEmpty()) {
            ESP_LOGE(TAG, "No token found in NVS. Use beginWithAgentCode() first.");
            return false;
        }
        char endpointBuf[256];
        snprintf(endpointBuf, sizeof(endpointBuf),
                 "wss://api.xiaozhi.me/mcp/?token=%s", token.c_str());
        mcpEndpoint = endpointBuf;
        ESP_LOGI(TAG, "Using saved token from NVS");
    }

    // Parse protocol
    const char* protocol_end = strstr(mcpEndpoint, "://");
    if (!protocol_end) {
        ESP_LOGE(TAG, "Invalid endpoint URL: missing '://'");
        return false;
    }

    if (strncmp(mcpEndpoint, "wss", 3) == 0) {
        _isSecure = true;
        // Inject embedded CA if connecting to XiaoZhi (avoids external secrets.h)
        if (_injectedClient && strstr(mcpEndpoint, "xiaozhi.me")) {
            ESP_LOGI(TAG, "Injecting embedded XiaoZhi root CA certificate");
            _injectedClient->setCACert(XIAOZHI_ROOT_CA);
        }
    } else if (strncmp(mcpEndpoint, "ws", 2) == 0) {
        _isSecure = false;
    } else {
        ESP_LOGE(TAG, "Unsupported protocol (must be 'ws' or 'wss')");
        return false;
    }

    if (_isSecure && !_injectedClient) {
        ESP_LOGE(TAG, "WSS requested but no secure client injected");
        return false;
    }

    // Parse host, port, path
    const char* host_start = protocol_end + 3;
    const char* path_start = strchr(host_start, '/');

    if (!path_start) {
        strncpy(_path, "/", sizeof(_path) - 1);
        _path[sizeof(_path) - 1] = '\0';
    } else {
        size_t path_len = strlen(path_start);
        size_t copy_len = std::min(path_len, sizeof(_path) - 1);
        strncpy(_path, path_start, copy_len);
        _path[copy_len] = '\0';
    }

    size_t host_port_len = path_start ? (path_start - host_start) : strlen(host_start);
    char host_port_temp[MAX_URL_LENGTH];
    if (host_port_len >= sizeof(host_port_temp)) {
        ESP_LOGE(TAG, "Host/port too long (max %u)", MAX_URL_LENGTH - 1);
        return false;
    }
    strncpy(host_port_temp, host_start, host_port_len);
    host_port_temp[host_port_len] = '\0';

    const char* port_sep = strchr(host_port_temp, ':');
    if (!port_sep) {
        _port = _isSecure ? 443 : 80;
        strncpy(_host, host_port_temp, sizeof(_host) - 1);
        _host[sizeof(_host) - 1] = '\0';
    } else {
        size_t host_len = port_sep - host_port_temp;
        if (host_len >= sizeof(_host)) {
            ESP_LOGE(TAG, "Host name too long (max %u)", MAX_URL_LENGTH - 1);
            return false;
        }
        strncpy(_host, host_port_temp, host_len);
        _host[host_len] = '\0';
        _port = atoi(port_sep + 1);
    }

    ESP_LOGI(TAG, "Parsed endpoint: host='%s', port=%u, path='%s'", _host, _port, _path);

    _lastReconnectAttempt = 0;
    _currentBackoff = INITIAL_BACKOFF;

    return true;
}

// === WebSocket Handshake (RFC 6455) ===

bool WebSocketMCP::performHandshake() {
    ESP_LOGI(TAG, "Starting WebSocket handshake with %s:%u", _host, _port);

    Client* netClient = _injectedClient;
    if (!netClient || !netClient->connected()) {
        ESP_LOGE(TAG, "No active network client for handshake");
        return false;
    }

    size_t len = 0;

    // 1. Generate Sec-WebSocket-Key (16 random bytes → Base64)
    uint8_t keyBytes[16];
    for (int i = 0; i < 16; i++) {
        keyBytes[i] = random(0, 256);
    }

    char keyBase64[25]; // 24 chars + null
    int ret = mbedtls_base64_encode(
        reinterpret_cast<unsigned char*>(keyBase64),
        sizeof(keyBase64), &len, keyBytes, 16
    );
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_base64_encode failed (key): %d", ret);
        return false;
    }
    keyBase64[len] = '\0';

    // 2. Build HTTP Upgrade request
    char requestBuffer[512];
    int requestLen = snprintf(requestBuffer, sizeof(requestBuffer),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "%s" // Optional Authorization header (not used in XiaoZhi flow)
        "\r\n",
        _path, _host, keyBase64,
        "" // ← Extend here if needed (e.g., "Authorization: Bearer ...\r\n")
    );

    if (requestLen < 0 || requestLen >= static_cast<int>(sizeof(requestBuffer))) {
        ESP_LOGE(TAG, "Handshake request overflow (size: %d)", requestLen);
        return false;
    }

    // 3. Send request
    netClient->write(reinterpret_cast<uint8_t*>(requestBuffer), requestLen);
    ESP_LOGD(TAG, "Handshake request sent:\n%.*s", requestLen, requestBuffer);

    // 4. Read response headers (max 2KB)
    unsigned long timeout = millis() + 5000;
    char responseBuffer[2048] = {0};
    int current_pos = 0;
    const char* terminator = "\r\n\r\n";
    const int term_len = 4;

    while (millis() < timeout && netClient->connected() && current_pos < static_cast<int>(sizeof(responseBuffer)) - 1) {
        if (netClient->available()) {
            char c = netClient->read();
            responseBuffer[current_pos++] = c;
            if (current_pos >= term_len &&
                memcmp(responseBuffer + current_pos - term_len, terminator, term_len) == 0) {
                break;
            }
        }
    }
    responseBuffer[current_pos] = '\0';

    if (!netClient->connected() || current_pos == 0) {
        ESP_LOGW(TAG, "Handshake timeout or connection lost");
        return false;
    }

    ESP_LOGD(TAG, "Server response:\n%s", responseBuffer);

    // 5. Check HTTP/1.1 101 Switching Protocols
    if (!strstr(responseBuffer, "HTTP/1.1 101")) {
        ESP_LOGE(TAG, "Invalid handshake response (expected 101)");
        netClient->stop();
        return false;
    }

    // 6. Validate Sec-WebSocket-Accept (optional but recommended)
    const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char combined[64];
    snprintf(combined, sizeof(combined), "%s%s", keyBase64, magic);

    uint8_t hash[20];
    ret = mbedtls_sha1(reinterpret_cast<const unsigned char*>(combined), strlen(combined), hash);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_sha1 failed: %d", ret);
        netClient->stop();
        return false;
    }

    char expectedAccept[30];
    ret = mbedtls_base64_encode(
        reinterpret_cast<unsigned char*>(expectedAccept),
        sizeof(expectedAccept), &len, hash, 20
    );
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_base64_encode failed (accept): %d", ret);
        netClient->stop();
        return false;
    }
    expectedAccept[len] = '\0';

    const char* acceptHeader = strstr(responseBuffer, "Sec-WebSocket-Accept: ");
    if (!acceptHeader) {
        ESP_LOGW(TAG, "Sec-WebSocket-Accept header missing (continuing anyway)");
    } else {
        acceptHeader += 22; // skip prefix
        char receivedAccept[30] = {0};
        strncpy(receivedAccept, acceptHeader, 28); // key is 28 chars

        if (strcmp(receivedAccept, expectedAccept) != 0) {
            ESP_LOGW(TAG, "Sec-WebSocket-Accept mismatch (expected: %s, got: %.28s)",
                     expectedAccept, receivedAccept);
            // Not fatal — some servers omit validation
        }
    }

    ESP_LOGI(TAG, "WebSocket handshake successful");
    _currentState = WS_CONNECTED;
    return true;
}

// === WebSocket Frame Sending (RFC 6455) ===

bool WebSocketMCP::sendWebSocketFrame(const char* data, size_t length, bool isText) {
    if (!_connected || !_injectedClient) {
        return false;
    }

    Client* netClient = _injectedClient;

    // Frame header (max: 2 + 8 + 4 = 14 bytes)
    uint8_t header[14];
    int headerLen = 0;

    // Masking key (required for clients)
    uint8_t maskingKey[4];
    for (int i = 0; i < 4; i++) {
        maskingKey[i] = random(0, 256);
    }

    // First byte: FIN=1, RSV=0, opcode
    uint8_t opcode = isText ? 0x01 : 0x0A; // TEXT or PONG
    if (length == 0 && !isText) {
        opcode = _currentState == WS_DISCONNECTED ? 0x08 : 0x0A; // CLOSE or PONG
    }
    header[headerLen++] = 0x80 | opcode;

    // Second byte: mask=1, payload length
    if (length <= 125) {
        header[headerLen++] = 0x80 | static_cast<uint8_t>(length);
    } else if (length <= 65535) {
        header[headerLen++] = 0x80 | 126;
        header[headerLen++] = (length >> 8) & 0xFF;
        header[headerLen++] = length & 0xFF;
    } else {
        ESP_LOGE(TAG, "Payload too large (>64KB): %u", length);
        return false;
    }

    // Append masking key
    memcpy(&header[headerLen], maskingKey, 4);
    headerLen += 4;

    // Send header
    if (netClient->write(header, headerLen) != headerLen) {
        ESP_LOGW(TAG, "Partial header write");
        return false;
    }

    // Send masked payload
    for (size_t i = 0; i < length; i++) {
        uint8_t masked = data[i] ^ maskingKey[i % 4];
        if (netClient->write(&masked, 1) != 1) {
            ESP_LOGW(TAG, "Failed to write payload byte %u", i);
            return false;
        }
    }

    netClient->flush();
    return true;
}

// === WebSocket Frame Receiving ===

size_t WebSocketMCP::receiveWebSocketFrame() {
    if (!_injectedClient || !_injectedClient->connected()) {
        return 0;
    }

    Client* netClient = _injectedClient;

    // Wait for at least 2 header bytes
    unsigned long timeout = millis() + 5000;
    while (netClient->available() < 2 && millis() < timeout) {
        delay(1);
    }
    if (netClient->available() < 2) {
        return 0;
    }

    // Read header
    uint8_t header1 = netClient->read();
    uint8_t header2 = netClient->read();

    bool fin = header1 & 0x80;
    uint8_t opcode = header1 & 0x0F;
    bool mask = header2 & 0x80;
    size_t payloadLen = header2 & 0x7F;

    // Handle control frames
    if (opcode == 0x08) { // CLOSE
        ESP_LOGI(TAG, "Received CLOSE frame");
        disconnect();
        return 0;
    }
    if (opcode == 0x0A) { // PONG
        ESP_LOGD(TAG, "Received PONG");
        _lastPingTime = millis();
        return 0;
    }
    if (opcode == 0x09) { // PING
        ESP_LOGD(TAG, "Received PING → sending PONG");
        sendWebSocketFrame("", 0, false);
        _lastPingTime = millis();
        return 0;
    }
    if (opcode != 0x01) { // Only TEXT supported
        ESP_LOGW(TAG, "Unsupported opcode: 0x%02X", opcode);
        return 0;
    }
    if (!fin) {
        ESP_LOGW(TAG, "Fragmented frames not supported");
        return 0;
    }

    // Extended payload length
    if (payloadLen == 126) {
        if (netClient->available() < 2) return 0;
        uint16_t len16 = (netClient->read() << 8) | netClient->read();
        payloadLen = len16;
    } else if (payloadLen == 127) {
        ESP_LOGE(TAG, "64-bit payload length not supported");
        for (int i = 0; i < 8; i++) netClient->read();
        return 0;
    }

    if (payloadLen >= MAX_MESSAGE_LENGTH) {
        ESP_LOGE(TAG, "Incoming payload too large (%u > %u)", payloadLen, MAX_MESSAGE_LENGTH);
        while (netClient->available() > 0) netClient->read();
        return 0;
    }

    // Read masking key (if present)
    uint8_t maskingKey[4] = {0};
    if (mask) {
        for (int i = 0; i < 4; i++) {
            if (netClient->available() == 0) return 0;
            maskingKey[i] = netClient->read();
        }
    }

    // Read payload into member buffer
    timeout = millis() + 5000;
    while (netClient->available() < payloadLen && millis() < timeout) {
        delay(1);
    }
    if (netClient->available() < payloadLen) {
        ESP_LOGE(TAG, "Timeout reading payload (%u expected)", payloadLen);
        return 0;
    }

    for (size_t i = 0; i < payloadLen; i++) {
        uint8_t b = netClient->read();
        if (mask) b ^= maskingKey[i % 4];
        _receiveBuffer[i] = static_cast<char>(b);
    }
    _receiveBuffer[payloadLen] = '\0';

    return payloadLen;
}

// === Message Processing ===

void WebSocketMCP::processReceivedData() {
    while (_injectedClient && _injectedClient->available()) {
        size_t len = receiveWebSocketFrame();
        if (len > 0) {
            handleJsonRpcMessage(_receiveBuffer, len);
        } else if (!_connected) {
            break;
        }
    }
}

// === JSON-RPC Message Handler ===

void WebSocketMCP::handleJsonRpcMessage(const char* message, size_t length) {
    // Use stack-allocated doc (256 bytes sufficient for tool responses)
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, message, length);

    if (err) {
        ESP_LOGW(TAG, "JSON parse failed: %s", err.c_str());
        return;
    }

    // Handle initialize response (activation flow)
    if (_awaitingActivation && doc.containsKey("result") && doc["id"] == 1) {
        if (doc["result"].containsKey("accessToken")) {
            const char* token = doc["result"]["accessToken"].as<const char*>();
            if (token && saveTokenToNVS(token)) {
                ESP_LOGI(TAG, "Activation successful — token saved to NVS");
                _awaitingActivation = false;

                // Reconnect with new token
                disconnect();
                delay(100);
                begin(nullptr, _connectionCallback); // nullptr → load from NVS
                return;
            } else {
                ESP_LOGE(TAG, "Failed to save accessToken from initialize response");
            }
        } else {
            ESP_LOGE(TAG, "initialize response missing accessToken");
        }
        _awaitingActivation = false;
        return;
    }

    // Handle tool invocation requests
    if (doc.containsKey("method")) {
        const char* method = doc["method"].as<const char*>();
        if (method && strcmp(method, "ToolInvocation") == 0) {
            if (doc.containsKey("params") && doc["params"].containsKey("tool_name")) {
                const char* toolName = doc["params"]["tool_name"].as<const char*>();
                const char* args = doc["params"]["parameters"].as<const char*>();

                if (!toolName || !args) {
                    ESP_LOGW(TAG, "ToolInvocation missing name or parameters");
                    return;
                }

                for (auto& tool : _tools) {
                    if (strcmp(tool.name, toolName) == 0) {
                        ToolResponse response = tool.callback(args);
                        ESP_LOGI(TAG, "Tool '%s' executed (error: %s)", toolName, response.isError() ? "yes" : "no");
                        return;
                    }
                }
                ESP_LOGW(TAG, "Tool not found: %s", toolName);
            }
        }
    }
}

// === Reconnection Logic ===

void WebSocketMCP::handleReconnect() {
    unsigned long now = millis();

    if (!_connected && (now - _lastReconnectAttempt > _currentBackoff || _lastReconnectAttempt == 0)) {
        _reconnectAttempt++;
        _lastReconnectAttempt = now;
        _currentBackoff = std::min(_currentBackoff * 2, MAX_BACKOFF);

        ESP_LOGW(TAG, "Reconnection attempt #%d (next delay: %.1fs)",
                 _reconnectAttempt, _currentBackoff / 1000.0f);

        Client* netClient = _injectedClient;
        if (!netClient) {
            ESP_LOGE(TAG, "No network client available for reconnection");
            return;
        }

        // Attempt TCP/TLS connection
        ESP_LOGI(TAG, "Connecting to %s:%u", _host, _port);
        if (!netClient->connect(_host, _port)) {
            ESP_LOGW(TAG, "TCP/TLS connection failed");
            return;
        }

        // Perform WebSocket handshake
        _currentState = WS_HANDSHAKING;
        if (performHandshake()) {
            _connected = true;
            _lastPingTime = millis();
            resetReconnectParams();

            ESP_LOGI(TAG, "WebSocket connected successfully");

            // If activation is pending, send initialize now
            if (_awaitingActivation && _pendingAgentCode[0] != '\0') {
                char initMsg[256];
                snprintf(initMsg, sizeof(initMsg),
                    "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\",\"params\":{\"agent_code\":\"%s\"},\"id\":1}",
                    _pendingAgentCode);
                sendMessage(initMsg, strlen(initMsg));
                ESP_LOGI(TAG, "Sent initialize message with agent code");
            }

            if (_connectionCallback) {
                _connectionCallback(true);
            }
        } else {
            netClient->stop();
            _currentState = WS_DISCONNECTED;
            ESP_LOGW(TAG, "WebSocket handshake failed");
        }
    }
}

void WebSocketMCP::resetReconnectParams() {
    _reconnectAttempt = 0;
    _currentBackoff = INITIAL_BACKOFF;
    _lastReconnectAttempt = 0;
}

// === Public Methods ===

bool WebSocketMCP::sendMessage(const char* message, size_t length) {
    if (!_connected) {
        ESP_LOGW(TAG, "Cannot send: not connected");
        return false;
    }

    ESP_LOGD(TAG, "Sending JSON-RPC message (%u bytes): %.64s...", length, message);
    return sendWebSocketFrame(message, length, true);
}

void WebSocketMCP::loop() {
    if (!_connected || !_injectedClient || !_injectedClient->connected()) {
        handleReconnect();
        return;
    }

    // Process incoming data
    if (_injectedClient->available()) {
        processReceivedData();
    }

    // Send keep-alive PING
    unsigned long now = millis();
    if (now - _lastPingTime > PING_INTERVAL) {
        ESP_LOGD(TAG, "Sending PING frame");
        sendWebSocketFrame("", 0, false);
        _lastPingTime = now;
    }

    // Timeout detection
    if (_lastPingTime > 0 && now - _lastPingTime > DISCONNECT_TIMEOUT) {
        ESP_LOGW(TAG, "Connection timeout — resetting");
        disconnect();
    }
}

bool WebSocketMCP::isConnected() {
    return _connected && _currentState == WS_CONNECTED;
}

void WebSocketMCP::disconnect() {
    if (_connected) {
        ESP_LOGI(TAG, "Disconnecting WebSocket");
        sendWebSocketFrame("", 0, false); // CLOSE frame
        if (_injectedClient) {
            _injectedClient->stop();
        }
        _connected = false;
        _currentState = WS_DISCONNECTED;
        _lastPingTime = 0;

        if (_connectionCallback) {
            _connectionCallback(false);
        }
    }
}

// === Tool Management ===

bool WebSocketMCP::registerTool(const char* name, const char* description,
                                const char* inputSchema, ToolCallback callback) {
    if (!name || !description || !inputSchema || !callback) {
        ESP_LOGE(TAG, "Tool registration failed: null parameter");
        return false;
    }

    Tool tool = {name, description, inputSchema, callback};
    _tools.push_back(tool);
    ESP_LOGI(TAG, "Registered tool: %s", name);
    return true;
}

bool WebSocketMCP::unregisterTool(const char* name) {
    for (auto it = _tools.begin(); it != _tools.end(); ++it) {
        if (strcmp(it->name, name) == 0) {
            _tools.erase(it);
            ESP_LOGI(TAG, "Unregistered tool: %s", name);
            return true;
        }
    }
    ESP_LOGW(TAG, "Tool not found for unregistration: %s", name);
    return false;
}

size_t WebSocketMCP::getToolCount() {
    return _tools.size();
}

void WebSocketMCP::clearTools() {
    _tools.clear();
    ESP_LOGI(TAG, "Cleared all tools (%u removed)", _tools.size());
}
