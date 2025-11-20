#include "WebSocketMCP.h"

#include <Arduino.h>
#include <Client.h>
#include "esp_log.h"
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"
#include <algorithm> 
#include <cstring>   
#include <cstdlib>   // For atoi

// Static constant definition
const int WebSocketMCP::INITIAL_BACKOFF = 1000;
const int WebSocketMCP::MAX_BACKOFF = 60000;
const int WebSocketMCP::PING_INTERVAL = 10000;
const int WebSocketMCP::DISCONNECT_TIMEOUT = 60000;

static const char *TAG = "MCP_WSS";

// Static instance pointer initialization
WebSocketMCP* WebSocketMCP::instance = nullptr;

// Constructor implementation
WebSocketMCP::WebSocketMCP(Client& client) : connected(false), lastReconnectAttempt(0),
currentBackoff(INITIAL_BACKOFF), reconnectAttempt(0), _injectedClient(&client),
_currentState(WebSocketMCP::WS_DISCONNECTED), _port(0), _isSecure(false) 
{
    instance = this;
    connectionCallback = nullptr;
    
    // CORREÇÃO ESTRUTURAL (LINHAS 32-34): Inicialização correta de membros array
    // O erro 'incompatible types in assignment of 'char' to 'char [N]'' é corrigido 
    // acessando o primeiro elemento do array ou usando memset.
    _host = '\0'; 
    _path = '\0';
    _receiveBuffer = '\0';
    
    ESP_LOGI(TAG,"Network Client injected.");
}

// ToolResponse Simplified Constructor 
ToolResponse::ToolResponse(bool err, const char* msg) : valid(true), error(err) {
    if (msg != nullptr) {
        DeserializationError doc_err = deserializeJson(doc, msg);
        if (doc_err) {
            valid = false;
        }
    }
    doc["success"] = !err;
}

// --- CORE NETWORKING AND PROTOCOL IMPLEMENTATION (Native C-Style) ---

/**
* @brief Attempts to connect the underlying TCP/TLS client and performs the WebSocket handshake.
* Uses fixed C-style buffers on the stack, eliminating String usage.
*/
bool WebSocketMCP::performHandshake() {
    ESP_LOGI(TAG, "Starting WebSocket Handshake.");

    Client* netClient = _injectedClient;

    if (!netClient || !netClient->connected()) {
        ESP_LOGE(TAG, "ERROR: No connected network client for handshake.");
        return false;
    }

    size_t len = 0;

    // 1. Generate Sec-WebSocket-Key (16 random bytes, Base64 encoded)
    // CORREÇÃO ESTRUTURAL: Declarar como array de 16 bytes
    uint8_t keyBytes; 
    for (int i = 0; i < 16; i++) {
        keyBytes[i] = random(0, 256);
    }

    // Base64 encoding de 16 bytes (24 chars). Usamos 30 para segurança.
    // CORREÇÃO ESTRUTURAL: Declarar como array de 30 bytes
    char keyBase64; 
    int ret = mbedtls_base64_encode((unsigned char*)keyBase64, sizeof(keyBase64), &len, keyBytes, 16);
    
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_base64_encode failed (Key Generation): %d.", ret);
        return false;
    }
    keyBase64[len] = '\0'; // Null-terminate
    ESP_LOGD(TAG, "Client WebSocket Key (Sec-WebSocket-Key): %s", keyBase64);

    // 2. Construct the Handshake Request (HTTP Upgrade)
    // Buffer seguro de 512 bytes para o cabeçalho HTTP completo.
    // CORREÇÃO ESTRUTURAL: Declarar como array de 512 bytes
    char requestBuffer; 
    
    int requestLen = snprintf(requestBuffer, sizeof(requestBuffer), 
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        _path, _host, keyBase64); 

    if (requestLen >= (int)sizeof(requestBuffer) || requestLen < 0) {
        ESP_LOGE(TAG, "ERROR: Handshake request buffer overflow or error (size: %d).", requestLen);
        return false;
    }

    // 3. Send Request
    netClient->write((uint8_t*)requestBuffer, requestLen);
    ESP_LOGI(TAG, "--- Handshake Request Sent ---");


    // 4. Read Response Headers (C-Style parsing required)
    unsigned long timeout = millis() + 5000;
    
    // Buffer seguro de 512 bytes para a resposta HTTP
    // CORREÇÃO ESTRUTURAL: Declarar como array de 512 bytes
    char responseBuffer = {0}; 
    int current_pos = 0;
    const char *terminator = "\r\n\r\n";
    const int term_len = 4;

    while (millis() < timeout && netClient->connected() && current_pos < sizeof(responseBuffer) - 1) {
        if (netClient->available()) {
            char c = netClient->read();
            responseBuffer[current_pos++] = c; 
            
            // Check for end of headers
            if (current_pos >= term_len && memcmp(responseBuffer + current_pos - term_len, terminator, term_len) == 0) {
                break;
            }
        }
    }
    responseBuffer[current_pos] = '\0'; // Null-terminate the response

    if (!netClient->connected() || current_pos == 0) {
        ESP_LOGW(TAG, "Handshake timeout or connection lost during response.");
        return false;
    }

    ESP_LOGD(TAG, "Raw Server Response Received:\n%s", responseBuffer);

    // 5. Validate Response Status (Check HTTP/1.1 101)
    if (strstr(responseBuffer, "HTTP/1.1 101") != responseBuffer) { 
        ESP_LOGE(TAG, "Invalid Handshake response code. Expected 101.");
        netClient->stop();
        return false;
    }

    // 6. Validate Sec-WebSocket-Accept
    const char* magicString = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    // Buffer combinado (KeyBase64 + Magic String). 65 bytes é seguro.
    // CORREÇÃO ESTRUTURAL: Declarar como array de 65 bytes
    char combined_buffer; 
    snprintf(combined_buffer, sizeof(combined_buffer), "%s%s", keyBase64, magicString);

    // SHA1 produces 20 bytes (160 bits)
    // CORREÇÃO ESTRUTURAL: Declarar como array de 20 bytes
    uint8_t hash; 
    ret = mbedtls_sha1((const unsigned char*)combined_buffer, strlen(combined_buffer), hash);

    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_sha1 calculation failed with code %d.", ret);
        netClient->stop();
        return false;
    }

    // Base64 encoding de 20 bytes (28 chars). 30 bytes é seguro.
    // CORREÇÃO ESTRUTURAL: Declarar como array de 30 bytes
    char expectedAcceptBase64; 
    ret = mbedtls_base64_encode((unsigned char*)expectedAcceptBase64, sizeof(expectedAcceptBase64), &len, hash, 20);
    
    if (ret != 0) {
         ESP_LOGE(TAG, "mbedtls_base64_encode failed (Accept Key): %d.", ret);
        netClient->stop();
        return false;
    }
    expectedAcceptBase64[len] = '\0';
    
    const char* expectedAccept = expectedAcceptBase64;

    ESP_LOGD(TAG, "Calculated Expected Accept Key: %s", expectedAccept);

    const char* acceptHeader = strstr(responseBuffer, "Sec-WebSocket-Accept: "); 
    
    if (acceptHeader == NULL) {
        ESP_LOGE(TAG, "Handshake failed: Sec-WebSocket-Accept header missing.");
        netClient->stop();
        return false;
    }

    // Extract the received key 
    acceptHeader += 22; // Skip "Sec-WebSocket-Accept: " (22 characters)
    // CORREÇÃO ESTRUTURAL: Declarar como array de 30 bytes
    char receivedAccept; 
    
    // Copy exactly 28 characters (key length) and null-terminate
    strncpy(receivedAccept, acceptHeader, 28);
    receivedAccept = '\0'; // Garantir terminação nula
    
    if (strcmp(receivedAccept, expectedAccept) != 0) {
        ESP_LOGE(TAG, "Validation Failed: Expected %s, Received %s.", expectedAccept, receivedAccept);
        netClient->stop();
        return false;
    }

    // Connection successful
    ESP_LOGI(TAG, "WebSocket Handshake SUCCESS!");
    _currentState = WebSocketMCP::WS_CONNECTED;
    return true;
}

/**
* @brief Implements WebSocket framing (RFC 6455) and sends data over the underlying client.
*/
bool WebSocketMCP::sendWebSocketFrame(const char* data, size_t payloadLength, bool isText) {
    if (!connected || !_injectedClient) {
        return false;
    }

    Client* netClient = _injectedClient;
    // Define header buffer on the stack (max size is 10 bytes: 1+1+8)
    // CORREÇÃO ESTRUTURAL: Declarar como array de 10 bytes
    uint8_t header; 
    int headerLen = 0;
    // CORREÇÃO ESTRUTURAL: Declarar como array de 4 bytes
    uint8_t maskingKey; 

    // 1. Generate Masking Key (Mandatory for client)
    for (int i = 0; i < 4; i++) {
        maskingKey[i] = random(0, 256);
    }
    
    // 2. First byte: FIN=1, RSV=0, Opcode=TEXT (0x1) or PONG (0xA) or CLOSE (0x8)
    uint8_t opcode = isText ? 0x01 : 0x0A; 
    if (payloadLength == 0 && !isText) {
        opcode = (_currentState == WS_DISCONNECTED) ? 0x08 : 0x0A; 
    }

    header[headerLen++] = 0x80 | opcode;

    // 3. Second byte: Mask=1, Payload Length
    if (payloadLength <= 125) {
        header[headerLen++] = 0x80 | (uint8_t)payloadLength;
    } else if (payloadLength <= 65535) {
        header[headerLen++] = 0x80 | 126;
        header[headerLen++] = (uint8_t)(payloadLength >> 8);
        header[headerLen++] = (uint8_t)(payloadLength & 0xFF);
    } else {
        ESP_LOGE(TAG, "ERROR: Payload length %u exceeds 64KB limit.", payloadLength);
        return false; 
    }
    
    // 4. Append Masking Key (4 bytes)
    // A chamada memcpy exige ponteiros válidos para buffers
    memcpy(header + headerLen, maskingKey, 4);
    headerLen += 4; 

    // 5. Send Header
    netClient->write(header, headerLen);

    // 6. Mask and Send Payload
    if (payloadLength > 0) {
        for (size_t i = 0; i < payloadLength; i++) {
            // Aplica XOR masking
            uint8_t maskedByte = data[i] ^ maskingKey[i % 4];
            netClient->write(maskedByte);
        }
    }

    netClient->flush();
    return true;
}

/**
* @brief Reads data from the socket, parses WebSocket frames, and extracts the payload 
* into the internal buffer _receiveBuffer.
*/
size_t WebSocketMCP::receiveWebSocketFrame() {
    if (!_injectedClient || !_injectedClient->connected()) {
        return 0;
    }

    Client* netClient = _injectedClient;

    unsigned long timeout = millis() + 5000;
    while (netClient->available() < 2 && millis() < timeout) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (netClient->available() < 2) {
        return 0;
    }

    // 1. Read first two header bytes
    uint8_t header1 = netClient->read();
    uint8_t header2 = netClient->read();

    bool fin = header1 & 0x80;
    uint8_t opcode = header1 & 0x0F;
    bool mask = header2 & 0x80; 

    size_t payloadLen = header2 & 0x7F;

    if (opcode == 0x08) { // CLOSE frame received
        ESP_LOGI(TAG,"Received CLOSE frame. Disconnecting.");
        disconnect();
        return 0;
    }

    if (opcode == 0x0A) { // PONG frame received
        ESP_LOGI(TAG,"Received PONG frame.");
        lastPingTime = millis();
        return 0;
    }

    if (opcode != 0x01) { 
        if (opcode == 0x09) { // Server PING received
            ESP_LOGI(TAG,"Received PING frame. Sending PONG.");
            sendWebSocketFrame("", 0, false); 
            lastPingTime = millis();
            return 0;
        }
        ESP_LOGW(TAG,"WARNING: Received unsupported opcode 0x%X", opcode);
        return 0;
    }

    if (!fin) {
        ESP_LOGW(TAG,"WARNING: Received fragmented frame (FIN=0). Skipping.");
        return 0;
    }

    // 2. Read Extended Length (if any)
    if (payloadLen == 126) {
        uint16_t len16 = netClient->read() << 8 | netClient->read();
        payloadLen = (size_t)len16;
    } else if (payloadLen == 127) {
        // Skip 64-bit length
        for(int i=0; i<8; i++) netClient->read(); 
        ESP_LOGE(TAG,"ERROR: Received 64-bit length payload (Unsupported).");
        return 0;
    }

    if (payloadLen >= MAX_MESSAGE_LENGTH) {
        ESP_LOGE(TAG, "ERROR: Incoming payload too large (%u bytes). Max: %u", payloadLen, MAX_MESSAGE_LENGTH);
        while(netClient->available() > 0) netClient->read(); 
        return 0;
    }


    // 3. Read Masking Key (if any)
    // CORREÇÃO ESTRUTURAL: Declarar como array de 4 bytes
    uint8_t maskingKey = {0}; 
    if (mask) {
        for (int i = 0; i < 4; i++) {
            maskingKey[i] = netClient->read();
        }
    }

    // 4. Read Payload (into internal fixed buffer)
    if (payloadLen == 0) {
        _receiveBuffer = '\0'; // Inicialização correta do array membro
        return 0;
    }
    
    timeout = millis() + 5000;
    while (netClient->available() < payloadLen && millis() < timeout) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (netClient->available() < payloadLen) {
        ESP_LOGE(TAG, "ERROR: Timeout while reading payload. Expected %u bytes.", payloadLen);
        return 0;
    }

    // Read and (if masked) unmask the payload directly into the buffer
    for (size_t i = 0; i < payloadLen; i++) {
        uint8_t b = netClient->read();
        if (mask) {
            b ^= maskingKey[i % 4];
        }
        _receiveBuffer[i] = (char)b;
    }
    _receiveBuffer[payloadLen] = '\0'; // Null-terminate
    return payloadLen;
}

/**
* @brief Processes incoming data from the socket by iterating through frames.
*/
void WebSocketMCP::processReceivedData() { 
    while (_injectedClient && _injectedClient->available()) {
        size_t message_length = receiveWebSocketFrame();

        if (message_length > 0) {
            handleJsonRpcMessage(_receiveBuffer, message_length);
        } else if (!connected) {
            break;
        }
    }
}

/**
* @brief Parses the endpoint URL and initializes host, port, and security settings.
*/
bool WebSocketMCP::begin(const char *mcpEndpoint, ConnectionCallback connCb) {
    connectionCallback = connCb;
    
    // 1. Determine Protocol (ws:// or wss://)
    const char *protocol_end = strstr(mcpEndpoint, "://");
    if (protocol_end == nullptr) {
        ESP_LOGE(TAG,"ERROR: Invalid endpoint URL format.");
        return false;
    }
    
    if (strncmp(mcpEndpoint, "wss", 3) == 0) {
        _isSecure = true;
    } else if (strncmp(mcpEndpoint, "ws", 2) == 0) {
        _isSecure = false;
    } else {
        ESP_LOGE(TAG,"ERROR: Unsupported protocol.");
        return false;
    }

    if (_isSecure && !_injectedClient) {
        ESP_LOGE(TAG,"ERROR: WSS requested but no Client object injected. TLS will fail.");
    }
    
    const char *host_start = protocol_end + 3;
    const char *path_start = strchr(host_start, '/');

    if (path_start == nullptr) {
        strncpy(_path, "/", sizeof(_path));
    } else {
        size_t path_len = strlen(path_start);
        strncpy(_path, path_start, std::min(path_len, sizeof(_path) - 1));
        _path[std::min(path_len, sizeof(_path) - 1)] = '\0';
    }
    
    size_t host_port_len = (path_start == nullptr) ? strlen(host_start) : (path_start - host_start);
    
    char host_port_temp[MAX_URL_LENGTH];
    if (host_port_len >= sizeof(host_port_temp)) {
        ESP_LOGE(TAG, "ERROR: Host/Port length exceeds buffer size.");
        return false;
    }
    strncpy(host_port_temp, host_start, host_port_len);
    host_port_temp[host_port_len] = '\0';

    const char *port_sep = strchr(host_port_temp, ':');
    
    if (port_sep == nullptr) {
        if (_isSecure) {
            _port = 443;
        } else {
            _port = 80;
        }
        strncpy(_host, host_port_temp, sizeof(_host));
    } else {
        size_t host_len = port_sep - host_port_temp;
        if (host_len >= sizeof(_host)) {
             ESP_LOGE(TAG, "ERROR: Host name too long.");
             return false;
        }
        strncpy(_host, host_port_temp, host_len);
        _host[host_len] = '\0';
        _port = atoi(port_sep + 1); 
    }

    ESP_LOGI(TAG,"Configuration: Host: %s, Port: %u, Path: %s", _host, _port, _path);

    lastReconnectAttempt = 0;
    currentBackoff = INITIAL_BACKOFF;

    return true;
}

bool WebSocketMCP::sendMessage(const char *message, size_t length) {
    if (!connected) {
        ESP_LOGW(TAG,"Not connected to WebSocket server, unable to send messages");
        return false;
    }

    ESP_LOGI(TAG, "Sending message (length %u)", length);

    if (sendWebSocketFrame(message, length, true)) {
        return true;
    }

    ESP_LOGE(TAG, "Failed to send WebSocket frame.");
    return false;
}

void WebSocketMCP::loop() {
    if (!connected || !_injectedClient || !_injectedClient->connected()) {
        handleReconnect();
        return;
    }

    if (connected && _injectedClient) {
        // 1. Process Incoming Data
        if (_injectedClient->available()) {
            processReceivedData(); 
        }

        // 2. Handle Keep-Alive (PING)
        unsigned long now = millis();
        if (now - lastPingTime > PING_INTERVAL) {
            ESP_LOGI(TAG,"Sending WebSocket PING frame.");
            sendWebSocketFrame("", 0, false); 
            lastPingTime = now;
        }

        // 3. Handle Disconnection Timeout
        if (lastPingTime > 0 && now - lastPingTime > DISCONNECT_TIMEOUT) {
            ESP_LOGW(TAG,"Ping/Inactivity timeout, resetting connection.");
            disconnect();
        }
    }
}

bool WebSocketMCP::isConnected() {
    return connected;
}

void WebSocketMCP::disconnect() {
    if (connected) {
        // Send CLOSE frame (Opcode 0x08)
        sendWebSocketFrame("", 0, false);

        if (_injectedClient) {
            _injectedClient->stop(); 
        }

        connected = false;
        lastPingTime = 0;
        _currentState = WebSocketMCP::WS_DISCONNECTED;

        ESP_LOGI(TAG,"WebSocket connection disconnected.");

        if (connectionCallback) {
            connectionCallback(false);
        }
    }
}

void WebSocketMCP::handleReconnect() {
    unsigned long now = millis();

    if (!connected && (now - lastReconnectAttempt > currentBackoff || lastReconnectAttempt == 0)) {
        reconnectAttempt++;
        lastReconnectAttempt = now;

        currentBackoff = std::min(currentBackoff * 2, MAX_BACKOFF);

        ESP_LOGW(TAG, "Trying to reconnect (attempts: %d, Next wait: %.2fs)",
            reconnectAttempt, currentBackoff / 1000.0);

        Client* netClient = _injectedClient;

        if (!netClient) {
            ESP_LOGE(TAG, "ERROR: Network client is NULL.");
            return;
        }

        // 1. Attempt TCP/TLS Connection
        ESP_LOGI(TAG, "Connecting to %s:%u...", _host, _port);

        if (netClient->connect(_host, _port)) {
            ESP_LOGI(TAG, "TCP/TLS connected. Performing WebSocket Handshake...");

            // 2. Perform WebSocket Handshake
            _currentState = WebSocketMCP::WS_HANDSHAKING;
            if (performHandshake()) {
                connected = true;
                resetReconnectParams();
                ESP_LOGI(TAG, "WebSocket is connected (Handshake Success)");
                if (connectionCallback) {
                    connectionCallback(true);
                }
                lastPingTime = millis();
            } else {
                netClient->stop();
                _currentState = WebSocketMCP::WS_DISCONNECTED;
                ESP_LOGW(TAG, "WebSocket Handshake failed.");
            }
        } else {
            ESP_LOGW(TAG, "TCP/TLS connection failed.");
        }
    }
}

void WebSocketMCP::resetReconnectParams() {
    reconnectAttempt = 0;
    currentBackoff = INITIAL_BACKOFF;
    lastReconnectAttempt = 0;
}

/**
* @brief Processes incoming JSON-RPC message (C-style buffer).
*/
void WebSocketMCP::handleJsonRpcMessage(const char *message, size_t length) {
    DynamicJsonDocument doc(1024); 
    DeserializationError error = deserializeJson(doc, message, length);

    if (error) {
        ESP_LOGE(TAG, "JSON deserialization failed: %s", error.c_str());
        return;
    }

    if (doc.containsKey("method")) {
        const char *method = doc["method"].as<const char*>();
        
        if (method != nullptr && strcmp(method, "ToolInvocation") == 0) {
            
            if (doc.containsKey("params") && doc["params"].containsKey("tool_name")) {
                const char *toolName = doc["params"]["tool_name"].as<const char*>();
                
                if (toolName == nullptr) return;

                for (auto& tool : _tools) {
                    if (strcmp(tool.name, toolName) == 0) {
                        const char *params = doc["params"]["parameters"].as<const char*>();
                        
                        ToolResponse response = tool.callback(params);
                        
                        if (response.isValid()) {
                            ESP_LOGI(TAG, "Tool Invocation processed: %s", toolName);
                        }
                        return;
                    }
                }
                ESP_LOGW(TAG, "Tool not registered: %s", toolName);
            }
        }
    }
}

bool WebSocketMCP::registerTool(const char *name, const char *description,
                                const char *inputSchema, ToolCallback callback) {
    Tool newTool = {name, description, inputSchema, callback};
    _tools.push_back(newTool);
    ESP_LOGI(TAG, "Tool registered: %s", name);
    return true;
}

bool WebSocketMCP::unregisterTool(const char *name) {
    for (size_t i = 0; i < _tools.size(); i++) {
        if (strcmp(_tools[i].name, name) == 0) {
            _tools.erase(_tools.begin() + i);
            ESP_LOGI(TAG, "Uninstalled tool: %s", name);
            return true;
        }
    }
    ESP_LOGW(TAG, "Tool %s does not exist, cannot be uninstalled", name);
    return false;
}

size_t WebSocketMCP::getToolCount() {
    return _tools.size();
}

void WebSocketMCP::clearTools() {
    _tools.clear();
    ESP_LOGI(TAG, "All tools have been cleared");
}

int WebSocketMCP::escapeJsonString(const char *input, char *output, size_t max_len) {
    if (input == nullptr) {
        if (max_len > 0) *output = '\0'; // CORREÇÃO ESTRUTURAL: Uso de dereferência
        return 0;
    }
    
    size_t input_len = strlen(input);
    if (input_len + 2 < max_len) { 
        *output = '"'; // CORREÇÃO ESTRUTURAL: Uso de dereferência
        strncpy(output + 1, input, max_len - 3);
        output[input_len + 1] = '"';
        output[input_len + 2] = '\0';
        return input_len + 2;
    }
    
    ESP_LOGE(TAG, "ERROR: JSON string escape buffer too small.");
    return 0;
}
