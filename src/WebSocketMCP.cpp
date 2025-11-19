
#include "WebSocketMCP.h"

// Static instance pointer initialization
WebSocketMCP* WebSocketMCP::instance = nullptr;

// Static constant definition
const int WebSocketMCP::INITIAL_BACKOFF;
const int WebSocketMCP::MAX_BACKOFF;
const int WebSocketMCP::PING_INTERVAL;
const int WebSocketMCP::DISCONNECT_TIMEOUT;

WebSocketMCP::WebSocketMCP() : connected(false), lastReconnectAttempt(0), 
                              currentBackoff(INITIAL_BACKOFF), reconnectAttempt(0) {
  // Setting static instance pointer
  instance = this;
  connectionCallback = nullptr;
  _injectedClient = nullptr; // Garantee that default constructor is null
}

WebSocketMCP::WebSocketMCP(Client& client) : connected(false), lastReconnectAttempt(0),
currentBackoff(INITIAL_BACKOFF), reconnectAttempt(0) {
    instance = this;
    connectionCallback = nullptr;
    // Salva a referência do cliente configurado com o CA Root
    _injectedClient = &client; 
    Serial.println("[MCP Secure] injected and ready to WSS.");
}

bool WebSocketMCP::begin(const char *mcpEndpoint,  ConnectionCallback connCb) {
  // Save the callback function
  connectionCallback = connCb;
  
  // Parse WebSocket URLs
  String url = String(mcpEndpoint);
  int protocolPos = url.indexOf("://");
  String protocol = url.substring(0, protocolPos);
  
  String remaining = url.substring(protocolPos + 3);
  int pathPos = remaining.indexOf('/');
  
  String host;
  String path = "/";
  int port = 80;
  
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
  
  if (protocol == "wss") {
      // Use secure client inserted
      if (_injectedClient) {
          // Usa a API da WebSocketsClient para definir o cliente TLS subjacente
          webSocket.setClient(*_injectedClient);
      }
      // A chamada beginSSL agora utiliza o cliente que foi injetado (se configurado)
      webSocket.beginSSL(host.c_str(), port, path.c_str());
  } else {
      webSocket.begin(host.c_str(), port, path.c_str());
  }
  
  // Set the disconnect timeout to 60 seconds
  // webSocket.setReconnectInterval(DISCONNECT_TIMEOUT);
  webSocket.enableHeartbeat(PING_INTERVAL, PING_INTERVAL, DISCONNECT_TIMEOUT);
  
  // Register event callback
  webSocket.onEvent(webSocketEvent);
  
  Serial.println("[xiaozhi-mcp] Connecting to the WebSocket server:" + url);
  return true;
}

void WebSocketMCP::webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  // Make sure the instance exists
  if (!instance) {
    return;
  }
  
  switch (type) {
    case WStype_DISCONNECTED:
      if (instance->connected) {
        instance->connected = false;
        Serial.println("[xiaozhi-mcp] WebSocket connection has been disconnected");
        if (instance->connectionCallback) {
          instance->connectionCallback(false);
        }
      }
      break;
      
    case WStype_CONNECTED:
      {
        instance->connected = true;
        instance->resetReconnectParams();
        Serial.println("[xiaozhi-mcp] WebSocket is connected");
        if (instance->connectionCallback) {
          instance->connectionCallback(true);
        }
      }
      break;
      
    case WStype_TEXT:
      {
        // Receive WebSocket message and process JSON-RPC request
        String message = String((char *)payload);
        instance->handleJsonRpcMessage(message);
      }
      break;
      
    case WStype_BIN:
      Serial.println("[xiaozhi-mcp] Received binary data, length:" + String(length));
      break;
      
    case WStype_ERROR:
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
      break;
  }
}

bool WebSocketMCP::sendMessage(const String &message) {
  if (!connected) {
    Serial.println("[xiaozhi-mcp] Not connected to WebSocket server, unable to send messages");
    return false;
  }
  // Send text messages to the WebSocket server (equivalent to stdin)
  Serial.println("[xiaozhi-mcp] Send message:" + message);
  String msg = message;
  webSocket.sendTXT(msg);
  return true;
}

void WebSocketMCP::loop() {
  // Handle WebSocket connections
  webSocket.loop();
  
  // Check if reconnection is required
  if (!connected) {
    handleReconnect();
  }
  
  // Handle possible ping timeouts
  if (connected && lastPingTime > 0) {
    unsigned long now = millis();
    // If you do not receive ping for more than 2 minutes, the connection may have been disconnected
    if (now - lastPingTime > 120000) {
      Serial.println("[xiaozhi-mcp] Ping timeout, reset connection");
      disconnect();
    }
  }
}

bool WebSocketMCP::isConnected() {
  return connected;
}

void WebSocketMCP::disconnect() {
  if (connected) {
    webSocket.disconnect();
    connected = false;
    lastPingTime = 0;
  }
}

void WebSocketMCP::handleReconnect() {
  // The WebSocket library already has an automatic reconnect function, which mainly deals with logs and notifications of reconnection status.
  unsigned long now = millis();
  if (!connected && (now - lastReconnectAttempt > currentBackoff || lastReconnectAttempt == 0)) {
    reconnectAttempt++;
    lastReconnectAttempt = now;
    
    // Calculate the waiting time for the next reconnect (exponent backoff)
    currentBackoff = min(currentBackoff * 2, MAX_BACKOFF);
    
    Serial.println("[xiaozhi-mcp] Trying to reconnect (number of attempts:" + String(reconnectAttempt) + 
        ", Next time waiting time:" + String(currentBackoff / 1000.0, 2) + "Second");
  }
}

void WebSocketMCP::resetReconnectParams() {
  reconnectAttempt = 0;
  currentBackoff = INITIAL_BACKOFF;
  lastReconnectAttempt = 0;
}

// Added a new method to process JSON-RPC messages
void WebSocketMCP::handleJsonRpcMessage(const String &message) {
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, message);
  
  if (error) {
    Serial.println("[xiaozhi-mcp] Failed to parse JSON:" + String(error.c_str()));
    return;
  }
  
  // Check if it is a ping request
  if (doc.containsKey("method") && doc["method"] == "ping") {
    // Record the last ping time
    lastPingTime = millis();
    
    // Construct pong response - Response with original id without modification
    String id = doc["id"].as<String>();
    Serial.println("[xiaozhi-mcp] Received a ping request:" + id);

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
    Serial.println("[xiaozhi-mcp] Respond to initialize request")alize request");
    
    // Send initialized notifications
    sendMessage("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
  }
  // Process tools/list requests
  else if (doc.containsKey("method") && doc["method"] == "tools/list") {
    String id = doc["id"].as<String>();
    
    // Here you can customize the tool list according to actual conditions
    String response = "{\"jsonrpc\":\"2.0\",\"id\":" + id + 
      ",\"result\":{\"tools\":[";
    
    // Generate tool information from the registered tool list
    if (_tools.size() > 0) {
      for (size_t i = 0; i < _tools.size(); i++) {
        if (i > 0) {
          response += ",";
        }
        response += "{\"name\":\"" + _tools[i].name + "\",";
        response += "\"description\":\"" + _tools[i].description + "\",";
        response += "\"inputSchema\":" + _tools[i].inputSchema + "}";
      }
    }
    
    response += "]}}";
    
    sendMessage(response);
    Serial.println("[xiaozhi-mcp] Respond to tools/list request, total" + String(_tools.size()) + "A tool");
  }
  // Process tools/call requests
  else if (doc.containsKey("method") && doc["method"] == "tools/call") {
    int id = doc["id"].as<int>();
    String toolName = doc["params"]["name"].as<String>();
    JsonObject arguments = doc["params"]["arguments"].as<JsonObject>();
    
    Serial.println("[xiaozhi-mcp] Received a tool call request:" + toolName);
    
    // Find Tools
    bool toolFound = false;
    ToolResponse toolResponse;
    
    for (size_t i = 0; i < _tools.size(); i++) {
      if (_tools[i].name == toolName) {
        toolFound = true;
        // Call the tool callback, pass in parameters and get the result
        if (_tools[i].callback) {
          String argumentsJson;
          serializeJson(arguments, argumentsJson);
          
          // Calling the callback and getting structured results
          toolResponse = _tools[i].callback(argumentsJson);
        } else {
          toolResponse = ToolResponse("{\"error\":\"Tool callback not registered\"}", true);
        }
        break;
      }
    }
    
    if (!toolFound) {
      toolResponse = ToolResponse("{\"error\":\"Tool not found: " + toolName + "\"}", true);
    }
    
    // Construct the response
    DynamicJsonDocument responseDoc(2048);
    responseDoc["jsonrpc"] = "2.0";
    responseDoc["id"] = id;
    
    JsonObject result = responseDoc.createNestedObject("result");
    
    JsonArray content = result.createNestedArray("content");
    for (const auto& item : toolResponse.content) {
      JsonObject contentItem = content.createNestedObject();
      contentItem["type"] = item.type;
      contentItem["text"] = item.text;
    }

    result["isError"] = toolResponse.isError;
    
    String response;
    serializeJson(responseDoc, response);
    
    sendMessage(response);
    Serial.println("[xiaozhi-mcp] Tool call complete:" + toolName + (toolResponse.isError ? "(Error)" : ""));
  }
}

// Escape special characters in JSON strings
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

// Add tool registration method - bring callback function version
bool WebSocketMCP::registerTool(const String &name, const String &description, 
                              const String &inputSchema, ToolCallback callback) {
  // Check if the tool already exists
  for (size_t i = 0; i < _tools.size(); i++) {
    if (_tools[i].name == name) {
      // If the tool exists, you can choose to update the callback
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

// Add a simplified tool registration method
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

// Uninstall tool
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

// Get the number of tools
size_t WebSocketMCP::getToolCount() {
  return _tools.size();
}

// Clear all tools
void WebSocketMCP::clearTools() {
  _tools.clear();
  Serial.println("[WebSocketMCP] All tools have been cleared") have been cleared");
}

// Format JSON strings, each key-value pair takes up one line
String WebSocketMCP::formatJsonString(const String &jsonStr) {
  // 1. Handle empty strings or invalid JSON
  if (jsonStr.length() == 0) {
    return "{}";
  }

  // 2. Try parsing JSON to make sure it works
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, jsonStr);
  
  if (error) {
    // If parsing fails, return to the original string
    return jsonStr;
  }
  
  // 3. Initialize the result string
  String result = "{\n";
  
  // 4. Get all keys in JSON
  JsonObject obj = doc.as<JsonObject>();
  bool firstItem = true;
  
  // 5. Iterate through each key-value pair in the JSON object
  for (JsonPair p : obj) {
    if (!firstItem) {
      result += ",\n";
    }
    firstItem = false;
    
    // Add two spaces indentation
    result += "  \"" + String(p.key().c_str()) + "\": ";
    
    // Add corresponding representation according to the type of value
    if (p.value().is<JsonObject>() || p.value().is<JsonArray>()) {
      // If the value is an object or an array, use serializeJson to convert
      String nestedJson;
      serializeJson(p.value(), nestedJson);
      result += nestedJson;
    } else if (p.value().is<const char*>() || p.value().is<String>()) {
      // If it is a string, add quotation marks
      result += "\"" + String(p.value().as<const char*>()) + "\"";
    } else {
      // For other types (numbers, boolean values, etc.)
      String valueStr;
      serializeJson(p.value(), valueStr);
      result += valueStr;
    }
  }
  
  // 6. Add end brackets (line wrap and close)
  result += "\n}";
  
  return result;
}

