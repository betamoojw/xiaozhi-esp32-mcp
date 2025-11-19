#include "WebSocketMCP.h"
#include <WiFi.h> 
#include <ArduinoJson.h>

// Include necessary cryptographic helpers typically available in the ESP32 Core,
// even if used internally by the old library, they are needed for manual handshake.
#include "mbedtls/sha1.h" // Assuming standard crypto headers from ESP-IDF are accessible
#include "mbedtls/base64.h" // For Base64 encoding/decoding

// Static instance pointer initialization
WebSocketMCP* WebSocketMCP::instance = nullptr;

// Static constant definition
const int WebSocketMCP::INITIAL_BACKOFF;
const int WebSocketMCP::MAX_BACKOFF;
const int WebSocketMCP::PING_INTERVAL;
const int WebSocketMCP::DISCONNECT_TIMEOUT;

// Default constructor implementation (retained for backward compatibility)
WebSocketMCP::WebSocketMCP() : connected(false), lastReconnectAttempt(0),
currentBackoff(INITIAL_BACKOFF), reconnectAttempt(0), _injectedClient(nullptr), _currentState(WS_DISCONNECTED) {
    // Setting static instance pointer
    instance = this;
    connectionCallback = nullptr;
}

// ✅ NEW CONSTRUCTOR IMPLEMENTATION: Saves the reference to the injected client.
WebSocketMCP::WebSocketMCP(Client& client) : connected(false), lastReconnectAttempt(0),
currentBackoff(INITIAL_BACKOFF), reconnectAttempt(0), _injectedClient(&client), _currentState(WS_DISCONNECTED) {
    instance = this;
    connectionCallback = nullptr;
    // Log the successful injection of the Client
    Serial.println("[xiaozhi-mcp] Network Client injected.");
}

// --- CORE NETWORKING AND PROTOCOL IMPLEMENTATION (Native) ---

/**
 * @brief Attempts to connect the underlying TCP/TLS client and performs the WebSocket handshake.
 * @return True if handshake successful, False otherwise.
 */
bool WebSocketMCP::performHandshake() {
    Client* netClient = _injectedClient; 

    if (!netClient) {
        // Should not happen if _isSecure is true, but check defensively
        Serial.println("[xiaozhi-mcp] ERROR: No network client for handshake.");
        return false;
    }

    // 1. Generate Sec-WebSocket-Key (16 random bytes, Base64 encoded)
    uint8_t keyBytes[4];
    for (int i = 0; i < 16; i++) {
        keyBytes[i] = random(0, 256);
    }

    char keyBase64[5]; // 16 bytes -> 24 chars Base64 + null terminator
    size_t len;
    mbedtls_base64_encode((unsigned char*)keyBase64, sizeof(keyBase64), &len, keyBytes, 16);
    keyBase64[len] = '\0'; // Ensure null termination
    String clientKey = String(keyBase64);
    
    // 2. Construct the Handshake Request (HTTP Upgrade)
    String handshakeRequest = 
        "GET " + _path + " HTTP/1.1\r\n"
        "Host: " + _host + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + clientKey + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        // Optional headers like Origin or protocol can be added here
        "\r\n"; // End of headers
    
    // 3. Send Request
    netClient->print(handshakeRequest);

    unsigned long timeout = millis() + 5000;
    String response = "";

    // 4. Read Response Headers (Wait for the empty line)
    while (millis() < timeout && netClient->connected()) {
        if (netClient->available()) {
            char c = netClient->read();
            response += c;
            if (response.endsWith("\r\n\r\n")) {
                break; // Found end of headers
            }
        }
    }

    if (!netClient->connected() || response.length() == 0) {
        Serial.println("[xiaozhi-mcp] Handshake timeout or connection lost during response.");
        return false;
    }

    // 5. Validate Response
    if (!response.startsWith("HTTP/1.1 101")) {
        Serial.println("[xiaozhi-mcp] Invalid Handshake response code. Expected 101.");
        return false;
    }
    if (response.indexOf("Upgrade: websocket") == -1 || 
        response.indexOf("Connection: Upgrade") == -1) {
        Serial.println("[xiaozhi-mcp] Missing Upgrade/Connection headers.");
        return false;
    }

    // 6. Validate Sec-WebSocket-Accept
    // Concatenate client key + magic string
    String magicString = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    String combined = clientKey + magicString;
    
    // Calculate SHA1 hash of the combined string
    uint8_t hash[6];
    mbedtls_sha1((const unsigned char*)combined.c_str(), combined.length(), hash);

    // Base64 encode the SHA1 hash
    char expectedAcceptBase64[7]; // 20 bytes -> 28 chars Base64 + null terminator
    mbedtls_base64_encode((unsigned char*)expectedAcceptBase64, sizeof(expectedAcceptBase64), &len, hash, 20);
    expectedAcceptBase64[len] = '\0';
    String expectedAccept = String(expectedAcceptBase64);

    if (response.indexOf("Sec-WebSocket-Accept: " + expectedAccept) == -1) {
        Serial.println("[xiaozhi-mcp] Handshake failed: Sec-WebSocket-Accept mismatch.");
        // Optional: Serial.println("Expected: " + expectedAccept);
        // Optional: Serial.println("Received: " + response.substring(response.indexOf("Sec-WebSocket-Accept: ")));
        return false;
    }

    _currentState = WS_CONNECTED;
    return true;
}

/**
 * @brief Implements WebSocket framing (RFC 6455) and sends data over the underlying client.
 * NOTE: This implementation includes mandatory client masking (M=1).
 */
bool WebSocketMCP::sendWebSocketFrame(const String& data, bool isText) {
    if (!connected || !_injectedClient) {
        return false;
    }

    Client* netClient = _injectedClient;
    size_t payloadLength = data.length();
    uint8_t header[3]; 
    int headerLen = 0;
    
    // 1. First byte: FIN=1, RSV=0, Opcode=TEXT (0x1) or PING (0x9) 
    uint8_t opcode = isText ? 0x01 : 0x09;
    header[headerLen++] = 0x80 | opcode; // FIN bit set + Opcode

    // 2. Second byte: Mask=1 (M=1 is mandatory for clients), Payload Len
    uint8_t mask = 0x80;
    if (payloadLength <= 125) {
        header[headerLen++] = mask | payloadLength; // Mask bit set + length (7 bits)
    } else if (payloadLength <= 65535) {
        header[headerLen++] = mask | 126; // Mask bit set + length = 126 (next 2 bytes are 16-bit length)
        header[headerLen++] = (payloadLength >> 8) & 0xFF;
        header[headerLen++] = payloadLength & 0xFF;
    } else {
        // Handle extended 64-bit length (not common on microcontrollers)
        Serial.println("[xiaozhi-mcp] ERROR: Payload too large for microcontroller.");
        return false;
    }

    // 3. Masking Key (4 random bytes)
    uint8_t maskingKey[2];
    for (int i = 0; i < 4; i++) {
        maskingKey[i] = random(0, 256);
        header[headerLen++] = maskingKey[i];
    }
    
    // 4. Send Header
    netClient->write(header, headerLen);

    // 5. Mask and Send Payload
    if (payloadLength > 0) {
        for (size_t i = 0; i < payloadLength; i++) {
            // Apply XOR masking before sending
            uint8_t maskedByte = data[i] ^ maskingKey[i % 4];
            netClient->write(maskedByte);
        }
    }
    
    return netClient->flush();
}

/**
 * @brief Reads data from the socket, parses WebSocket frames, and extracts the payload.
 * Handles single frame text messages.
 * NOTE: This implementation is simplified and assumes non-fragmented, single-frame TEXT data.
 */
String WebSocketMCP::receiveWebSocketFrame() {
    if (!_injectedClient || !_injectedClient->connected()) {
        return "";
    }
    
    Client* netClient = _injectedClient;
    
    // Wait until at least 2 bytes (minimum header size) are available
    unsigned long timeout = millis() + 5000;
    while (netClient->available() < 2 && millis() < timeout) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    if (netClient->available() < 2) {
        return "";
    }

    // 1. Read first two header bytes
    uint8_t header1 = netClient->read(); // FIN, RSV, Opcode
    uint8_t header2 = netClient->read(); // Mask, Payload Length

    bool fin = header1 & 0x80;
    uint8_t opcode = header1 & 0x0F;
    bool mask = header2 & 0x80; // Should be 0 for server response
    size_t payloadLen = header2 & 0x7F;
    
    if (opcode == 0x08) { // CLOSE frame received
        Serial.println("[xiaozhi-mcp] Received CLOSE frame. Disconnecting.");
        disconnect();
        return "";
    }
    if (opcode == 0x0A) { // PONG frame received
        Serial.println("[xiaozhi-mcp] Received PONG frame.");
        // PONG confirms connectivity, update lastPingTime
        lastPingTime = millis();
        return "";
    }
    if (opcode != 0x01 && opcode != 0x09) { // Expecting TEXT (0x1) or PING (0x9)
        if (opcode == 0x09) {
            // Server PING received, send PONG response (opcode 0x0A)
            sendWebSocketFrame("", false); // Send empty PONG frame
            lastPingTime = millis();
            return "";
        }
        Serial.printf("[xiaozhi-mcp] WARNING: Received unsupported opcode 0x%X\n", opcode);
        return ""; 
    }
    if (!fin) {
        // Handle fragmentation if needed, but for simplicity, we assume single frame
        Serial.println("[xiaozhi-mcp] WARNING: Received fragmented frame (FIN=0).");
        return ""; 
    }

    // 2. Read Extended Length (if any)
    if (payloadLen == 126) {
        uint16_t len16 = netClient->read() << 8 | netClient->read();
        payloadLen = (size_t)len16;
    } else if (payloadLen == 127) {
        // 64-bit length (unsupported here)
        Serial.println("[xiaozhi-mcp] ERROR: Received 64-bit length payload.");
        return "";
    }

    // 3. Read Masking Key (if any)
    uint8_t maskingKey[2] = {0, 0, 0, 0};
    if (mask) {
        // Server response should NOT be masked, but read defensively if protocol violated
        for (int i = 0; i < 4; i++) {
            maskingKey[i] = netClient->read();
        }
    }

    // 4. Read Payload
    if (payloadLen == 0) {
        return "";
    }

    // Wait for the entire payload to be available
    while (netClient->available() < payloadLen && millis() < timeout) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    if (netClient->available() < payloadLen) {
        Serial.println("[xiaozhi-mcp] ERROR: Incomplete payload received.");
        return "";
    }

    String payload = "";
    payload.reserve(payloadLen + 1);

    for (size_t i = 0; i < payloadLen; i++) {
        uint8_t b = netClient->read();
        // Unmask if necessary (should be rare for server response)
        if (mask) {
            b ^= maskingKey[i % 4];
        }
        payload += (char)b;
    }

    return payload;
}


/**
 * @brief Processes incoming data from the socket by iterating through frames.
 */
void WebSocketMCP::processReceivedData() {
    // Continuously process frames while data is available
    while (_injectedClient && _injectedClient->available()) {
        String message = receiveWebSocketFrame();
        
        if (message.length() > 0) {
            // Received a valid WebSocket message, handle as JSON-RPC
            handleJsonRpcMessage(message);
        } else if (!connected) {
            // Disconnect occurred during frame reading (e.g., received CLOSE frame)
            break;
        }
    }
}


bool WebSocketMCP::begin(const char *mcpEndpoint, ConnectionCallback connCb) {

    // Save the callback function
    connectionCallback = connCb;

    // Parse WebSocket URLs
    String url = String(mcpEndpoint);
    int protocolPos = url.indexOf("://");
    if (protocolPos == -1) {
        Serial.println("[xiaozhi-mcp] ERROR: Invalid endpoint URL format.");
        return false;
    }

    String protocol = url.substring(0, protocolPos);
    String remaining = url.substring(protocolPos + 3);

    int pathPos = remaining.indexOf('/');
    String host;
    String path = "/";
    uint16_t port = 80;

    if (pathPos >= 0) {
        host = remaining.substring(0, pathPos);
        path = remaining.substring(pathPos);
    } else {
        host = remaining;
    }

    // Check port
    int portPos = host.indexOf(':');
    if (portPos >= 0) {
        port = host.substring(portPos + 1).toInt();
        host = host.substring(0, portPos);
    } else {
        // Set the default port according to the protocol
        if (protocol == "ws") {
            port = 80;
        } else if (protocol == "wss") {
            port = 443;
        }
    }
    
    // ✅ NEW: Save connection details to internal members
    _host = host;
    _port = port;
    _path = path;
    _isSecure = (protocol == "wss");
    
    // Check if client injection is required for WSS
    if (_isSecure && !_injectedClient) {
        Serial.println("[xiaozhi-mcp] ERROR: WSS requested but no Client object injected. TLS will fail.");
        // Allow configuration to complete, but reconnection logic will fail the connection attempt.
    }
    
    // Set the initial reconnection state to trigger immediate attempt in loop()
    lastReconnectAttempt = 0;
    currentBackoff = INITIAL_BACKOFF;
    
    // ✅ REMOVED: All calls to webSocket.beginSSL/begin() and webSocket.onEvent are removed.

    Serial.println("[xiaozhi-mcp] Configuration complete. Connection attempt delegated to loop.");

    return true;
}

// ✅ REMOVED: The entire webSocketEvent static handler is removed as it belongs to the old library.
// void WebSocketMCP::webSocketEvent(WStype_t type, uint8_t *payload, size_t length) { ... }


bool WebSocketMCP::sendMessage(const String &message) {
    if (!connected) {
        Serial.println("[xiaozhi-mcp] Not connected to WebSocket server, unable to send messages");
        return false;
    }

    // Send text messages to the WebSocket server (equivalent to stdin)
    Serial.println("[xiaozhi-mcp] Send message:" + message);

    // ✅ NEW: Use manual WebSocket framing over the Client socket
    if (sendWebSocketFrame(message, true)) {
        return true;
    }

    // Fallback if framing/writing failed
    Serial.println("[xiaozhi-mcp] Failed to send WebSocket frame.");
    return false;
}


void WebSocketMCP::loop() {
    
    // ✅ REMOVED: webSocket.loop() is removed.
    
    // Handle manual reconnection check
    if (!connected || !_injectedClient || !_injectedClient->connected()) {
        handleReconnect();
        return;
    }
    
    // If connected and client is valid
    if (connected && _injectedClient) {
        
        // 1. Process Incoming Data
        if (_injectedClient->available()) {
            processReceivedData();
        }
        
        // 2. Handle Keep-Alive (PING)
        unsigned long now = millis();
        if (now - lastPingTime > PING_INTERVAL) {
            // Note: PING frame uses Opcode 0x09
            Serial.println("[xiaozhi-mcp] Sending WebSocket PING frame.");
            sendWebSocketFrame("", false); 
            lastPingTime = now;
        }

        // 3. Handle Disconnection Timeout (if no PONG/data received)
        if (lastPingTime > 0 && now - lastPingTime > DISCONNECT_TIMEOUT) {
            Serial.println("[xiaozhi-mcp] Ping/Inactivity timeout, resetting connection.");
            disconnect();
        }
    }
}


bool WebSocketMCP::isConnected() {
    return connected;
}


void WebSocketMCP::disconnect() {
    if (connected) {
        // Send CLOSE frame (Opcode 0x08) before stopping the connection.
        // For simplicity, we skip payload for now.
        sendWebSocketFrame("", false); // Use custom framing for CLOSE frame
        
        if (_injectedClient) {
            _injectedClient->stop(); // Close the underlying TCP/TLS connection
        }
        connected = false;
        lastPingTime = 0;
        _currentState = WS_DISCONNECTED;

        Serial.println("[xiaozhi-mcp] WebSocket connection disconnected.");
        if (connectionCallback) {
            connectionCallback(false);
        }
    }
}


void WebSocketMCP::handleReconnect() {
    unsigned long now = millis();

    // Check if we are disconnected and the backoff time has passed
    if (!connected && (now - lastReconnectAttempt > currentBackoff || lastReconnectAttempt == 0)) {
        reconnectAttempt++;
        lastReconnectAttempt = now;

        // Calculate the waiting time for the next reconnect (exponent backoff)
        currentBackoff = min(currentBackoff * 2, MAX_BACKOFF);

        Serial.println("[xiaozhi-mcp] Trying to reconnect (number of attempts:" + String(reconnectAttempt) +
                        ", Next time waiting time:" + String(currentBackoff / 1000.0, 2) + "s)");

        Client* netClient = _injectedClient; 

        if (_isSecure && !netClient) {
             Serial.println("[xiaozhi-mcp] ERROR: Secure WSS requested but network client is NULL (must be injected).");
             return;
        }
        if (!netClient) {
             Serial.println("[xiaozhi-mcp] ERROR: No valid network client available.");
             return;
        }


        // 1. Attempt TCP/TLS Connection
        Serial.printf("[xiaozhi-mcp] Connecting to %s:%u...\n", _host.c_str(), _port);
        if (netClient->connect(_host.c_str(), _port)) {
            Serial.println("[xiaozhi-mcp] TCP/TLS connected. Performing WebSocket Handshake...");
            
            // 2. Perform WebSocket Handshake
            if (performHandshake()) { 
                connected = true;
                resetReconnectParams();
                Serial.println("[xiaozhi-mcp] WebSocket is connected (Handshake Success)");
                if (connectionCallback) {
                    connectionCallback(true);
                }
                // We set the initial ping time here as part of the connection success
                lastPingTime = millis();
            } else {
                netClient->stop();
                _currentState = WS_DISCONNECTED;
                Serial.println("[xiaozhi-mcp] WebSocket Handshake failed.");
            }
        } else {
            Serial.println("[xiaozhi-mcp] TCP/TLS connection failed.");
        }
    }
}


void WebSocketMCP::resetReconnectParams() {
    reconnectAttempt = 0;
    currentBackoff = INITIAL_BACKOFF;
    lastReconnectAttempt = 0;
}


// Added a new method to process JSON-RPC messages (Logic retained from original)
void WebSocketMCP::handleJsonRpcMessage(const String &message) {

    DynamicJsonDocument doc(1024); // Check buffer size if large responses are expected [8]

    DeserializationError error = deserializeJson(doc, message);

    if (error) {
        Serial.println("[xiaozhi-mcp] Failed to parse JSON:" + String(error.c_str()));
        return;
    }

    // Check if it is a ping request (MCP keep-alive, distinct from WebSocket PING/PONG)
    if (doc.containsKey("method") && doc["method"] == "ping") {
        // Record the last ping time
        lastPingTime = millis(); // Update heartbeat time

        // Construct pong response - Response with original id without modification
        String id = doc["id"].as<String>();

        Serial.println("[xiaozhi-mcp] Received a ping request:" + id);

        // Note: Using String concatenation for simplicity, but DynamicJsonDocument is safer for complex payloads
        String response = "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{}}"; 

        sendMessage(response);
        Serial.println("[xiaozhi-mcp] Respond to ping request:" + id);
    }

    // Process initialization request
    else if (doc.containsKey("method") && doc["method"] == "initialize") {
        String id = doc["id"].as<String>();
        String serverName = "ESP-HA";

        // Send initialization response
        String response = "{\"jsonrpc\":\"2.0\",\"id\":" + id +
            ",\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"experimental\":{},\"prompts\":{\"listChanged\":false},\"resources\":{\"subscribe\":false,\"listChanged\":false},\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"" + serverName + "\",\"version\":\"1.0.0\"}}}";

        sendMessage(response);

        Serial.println("[xiaozhi-mcp] Respond to initialize request");

        // Send initialized notifications
        sendMessage("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    }
    
    // Process tool invocation request
    else if (doc.containsKey("method") && doc["method"] == "tools/invoke") {
        // Retrieve tool name, arguments, and ID
        String toolName = doc["params"]["tool_name"].as<String>();
        String toolId = doc["id"].as<String>();
        
        // Convert parameters to ToolParams structure
        JsonVariantConst paramsVariant = doc["params"]["arguments"];
        String paramsJson;
        serializeJson(paramsVariant, paramsJson);
        
        Serial.println("[xiaozhi-mcp] Received tool invoke: " + toolName);

        ToolResponse toolResult;
        bool toolFound = false;

        for (const auto& tool : _tools) {
            if (tool.name == toolName) {
                toolFound = true;
                // Execute callback with raw JSON arguments
                toolResult = tool.callback(paramsJson);
                break;
            }
        }

        if (toolFound) {
            // Construct JSON-RPC result response
            String contentStr;
            // Assuming ToolResponse::content contains the data structure needed by MCP
            if (!toolResult.content.empty()) {
                // For simplicity, serialize only the first item's text, which is usually the full JSON response
                contentStr = toolResult.content.text; 
            } else {
                 contentStr = "{\"success\":true}"; // Default success if empty
            }
            
            // Build the final JSON-RPC response message
            String response = "{\"jsonrpc\":\"2.0\",\"id\":" + toolId + 
                              ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":" + escapeJsonString(contentStr) + 
                              "}],\"isError\":" + (toolResult.isError ? "true" : "false") + "}}";
            
            sendMessage(response);
            Serial.println("[xiaozhi-mcp] Tool response sent.");
        } else {
            // Tool not found error
            String errorMsg = "Tool not found: " + toolName;
            String response = "{\"jsonrpc\":\"2.0\",\"id\":" + toolId + 
                              ",\"error\":{\"code\":-32601,\"message\":\"" + errorMsg + "\"}}";
            sendMessage(response);
            Serial.println("[xiaozhi-mcp] Tool not found error sent.");
        }
    }


    // Process tools/list requests
    else if (doc.containsKey("method") && doc["method"] == "tools/list") {

        String id = doc["id"].as<String>();

        // Start construction of the tool list JSON array
        String toolArrayContent = "";
        bool firstTool = true;

        for (const auto& tool : _tools) {
            if (!firstTool) {
                toolArrayContent += ",";
            }
            // Build the tool object structure
            toolArrayContent += "{\"name\":\"" + tool.name + "\",\"description\":\"" + 
                                escapeJsonString(tool.description) + "\",\"inputSchema\":" + 
                                tool.inputSchema + "}"; 
            firstTool = false;
        }

        // Construct final JSON-RPC response
        String response = "{\"jsonrpc\":\"2.0\",\"id\":" + id +
                          ",\"result\":{\"tools\":[" + toolArrayContent + "]}}";

        sendMessage(response);
        Serial.println("[xiaozhi-mcp] Respond to tools/list request");

    } else {
        Serial.println("[xiaozhi-mcp] Received unhandled JSON-RPC message.");
    }
}


// Escape special characters in JSON strings (Logic retained from original)
String WebSocketMCP::escapeJsonString(const String &input) {

    String result = "";

    for (size_t i = 0; i < input.length(); i++) {

        char c = input[i];

        if (c == '\"' || c == '\\' || c == '/' ||
            c == '\b' || c == '\f' || c == '\n' ||
            c == '\r' || c == '\t') {

            if (c == '\"') result += "\\\"";
            else if (c == '\\') result += "\\\\";
            else if (c == '/') result += "\\/";
            else if (c == '\b') result += "\\b";
            else if (c == '\f') result += "\\f";
            else if (c == '\n') result += "\\n";
            else if (c == '\r') result += "\\r";
            else if (c == '\t') result += "\\t";
        } else {
            result += c;
        }
    }

    return result;

}

// Add tool registration method - bring callback function version (Logic retained from original)
bool WebSocketMCP::registerTool(const String &name, const String &description,
                                const String &inputSchema, ToolCallback callback) {

    // Check if the tool already exists
    for (size_t i = 0; i < _tools.size(); i++) {
        if (_tools[i].name == name) {
            // If the tool exists, update the callback
            _tools[i].callback = callback;
            Serial.println("[xiaozhi-mcp] Update tool callback:" + name);
            return true;
        }
    }

    // Create a new tool and add it to the list
    Tool newTool;
    newTool.name = name;
    newTool.description = description;
    newTool.inputSchema = inputSchema;
    newTool.callback = callback;
    _tools.push_back(newTool);

    Serial.println("[xiaozhi-mcp] Successful registration tool:" + name);

    return true;
}

// Add a simplified tool registration method (Logic retained from original)
bool WebSocketMCP::registerSimpleTool(const String &name, const String &description,
                                        const String &paramName, const String &paramDesc,
                                        const String &paramType, ToolCallback callback) {

    // Build a simple inputSchema
    String inputSchema = "{\"type\":\"object\",\"properties\":{\"" +
                        paramName + "\":{\"type\":\"" + paramType +
                        "\",\"description\":\"" + paramDesc +
                        "\"}},\"required\":[\"" + paramName + "\"]}";

    return registerTool(name, description, inputSchema, callback);
}

// Uninstall tool (Logic retained from original)
bool WebSocketMCP::unregisterTool(const String &name) {

    for (size_t i = 0; i < _tools.size(); i++) {

        if (_tools[i].name == name) {

            _tools.erase(_tools.begin() + i);

            Serial.println("[xiaozhi-mcp] Uninstalled tool:" + name);

            return true;
        }
    }

    Serial.println("[xiaozhi-mcp] Tools" + name + "Does not exist, cannot be uninstalled");
    return false;
}

// Get the number of tools (Logic retained from original)
size_t WebSocketMCP::getToolCount() {
    return _tools.size();
}

// Clear all tools (Logic retained from original)
void WebSocketMCP::clearTools() {
    _tools.clear();
    Serial.println("[WebSocketMCP] All tools have been cleared");
}

// Format JSON strings, each key-value pair takes up one line
String WebSocketMCP::formatJsonString(const String &jsonStr) {

    // 1. Handle empty strings or invalid JSON [1]
    if (jsonStr.length() == 0) {
        return "{}";
    }

    // 2. Try parsing JSON to make sure it works [1]
    DynamicJsonDocument doc(1024); // Size based on source fragment [1]
    DeserializationError error = deserializeJson(doc, jsonStr);

    if (error) {
        // If parsing fails, return to the original string [1]
        return jsonStr;
    }

    // 3. Initialize the result string [1]
    String result = "{\n";

    // 4. Get all keys in JSON [1]
    JsonObject obj = doc.as<JsonObject>(); // Explicit conversion for iteration
    bool firstItem = true;

    // 5. Iterate through each key-value pair in the JSON object [2]
    for (JsonPair p : obj) {

        if (!firstItem) {
            result += ",\n"; // Add comma and newline [2]
        }

        firstItem = false;

        // Add two spaces indentation [2]
        result += "  \"" + String(p.key().c_str()) + "\": ";

        // Add corresponding representation according to the type of value [2]
        if (p.value().is<JsonObject>() || p.value().is<JsonArray>()) {
            // If the value is an object or an array, use serializeJson to convert [2]
            String nestedJson;
            serializeJson(p.value(), nestedJson);
            result += nestedJson;
        } else if (p.value().is<String>() || p.value().is<const char*>()) { // Check for string types [3]
            // If it is a string, add quotation marks [3]
            result += "\"" + String(p.value().as<String>()) + "\"";
        } else {
            // For other types (numbers, boolean values, etc.) [3]
            String valueStr;
            serializeJson(p.value(), valueStr);
            result += valueStr;
        }

    }

    // 6. Add end brackets (line wrap and close) [3]
    result += "\n}";

    return result;
}
