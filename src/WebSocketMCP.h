

#ifndef WEBSOCKET_MCP_H
#define WEBSOCKET_MCP_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>  // This library needs to be added to parse JSON
#include <vector>
#include <functional>

/* *
 * WebSocketMCP Class
 * Encapsulates WebSocket connection and communication with MCP server */
class WebSocketMCP {
public:
  // Define the tool response content structure
  //Sending to WebSocket: {"jsonrpc":"2.0","id":48,"result":{"content":[{"type":"text","text":"{\n  \"success\": true,\n  \"result\": 2\n}"}],"isError":false}}
  struct ToolContentItem {
    String type;  // Content type, such as "text"
    String text;  // Text content
  };
  
  // Define tool response structure
  struct ToolResponse {
    std::vector<ToolContentItem> content;  // Response content array
    bool isError;                          // Is it an error response
    
    // Constructor: Create a response from a single text content
    ToolResponse(const String& textContent, bool error = false) {
      ToolContentItem item;
      item.type = "text";
      
      // Try to detect if it is JSON and format it (by finding the beginning { and ending })
      String trimmedText = textContent;
      trimmedText.trim();
      
      if (trimmedText.startsWith("{") && trimmedText.endsWith("}")) {
        // FormatJsonString method using WebSocketMCP
        // Since this is within a static method, we need to create a temporary instance to call a non-static method
        WebSocketMCP* instance = WebSocketMCP::instance;
        if (instance) {
          item.text = instance->formatJsonString(textContent);
        } else {
          item.text = textContent;
        }
      } else {
        item.text = textContent;
      }
      
      content.push_back(item);
      isError = error;
    }

    // New constructor: used to handle parameters of bool and String types
    ToolResponse(bool error, const String& message) {
      ToolContentItem item;
      item.type = "text";
      item.text = message;
      content.push_back(item);
      isError = error;
    }
    
    // Default constructor
    ToolResponse() : isError(false) {}
    
    // Create a response from a JSON object (convenient method)
    static ToolResponse fromJson(const JsonObject& json, bool error = false) {
      String jsonStr;
      serializeJson(json, jsonStr);
      return ToolResponse(jsonStr, error);
    }
  };

  // Auxiliary class for parameter processing
  class ToolParams {
  public:
    ToolParams(const String& json) {
      DeserializationError error = deserializeJson(doc, json);
      valid = !error;
    }

    // NEW: Static factory method from JsonVariantConst
    // This creates a new ToolParams instance by serializing the variant and re-parsing it.
    // Useful for handling items from JsonArray or nested JsonObjects reliably.
    static ToolParams fromVariant(const JsonVariantConst& variant) {
      String variantJson;
      serializeJson(variant, variantJson);
      return ToolParams(variantJson);
    }
    
    template<typename T>
    T get(const String& key, T defaultValue) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key)) {
        return defaultValue;
      }
      return doc.as<JsonObjectConst>()[key].as<T>();
    }
    
    JsonVariantConst getJsonValue(const String& key) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key)) {
        return JsonVariantConst();
      }
      return doc.as<JsonObjectConst>()[key];
    }
    
    JsonArrayConst getJsonArray(const String& key) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key) || !doc.as<JsonObjectConst>()[key].is<JsonArray>()) {
        return JsonArrayConst();
      }
      return doc.as<JsonObjectConst>()[key].as<JsonArrayConst>();
    }
    
    bool isArray(const String& key) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key)) {
        return false;
      }
      return doc.as<JsonObjectConst>()[key].is<JsonArray>();
    }
    
    size_t getArraySize(const String& key) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key) || !doc.as<JsonObjectConst>()[key].is<JsonArray>()) {
        return 0;
      }
      return doc.as<JsonObjectConst>()[key].as<JsonArrayConst>().size();
    }

    // NEW: Method to check if the root of this ToolParams' document is a JSON object
    bool isJsonObject() const {
      return valid && doc.is<JsonObject>();
    }

    // NEW: Method to check if the root of this ToolParams' document is a JSON array
    bool isJsonArray() const {
      return valid && doc.is<JsonArray>();
    }

    // NEW: Method to get the root as a JsonObjectConst if it is one
    JsonObjectConst getAsJsonObject() const {
      if (isJsonObject()) {
        return doc.as<JsonObjectConst>();
      }
      return JsonObjectConst(); // Returns a null JsonObjectConst
    }

    // NEW: Method to get the root as a JsonArrayConst if it is one
    JsonArrayConst getAsJsonArray() const {
      if (isJsonArray()) {
        return doc.as<JsonArrayConst>();
      }
      return JsonArrayConst(); // Returns a null JsonArrayConst
    }
    
    bool contains(const String& key) const {
      return valid && doc.is<JsonObject>() && doc.as<JsonObjectConst>().containsKey(key);
    }
    
    String getDebugJson() const {
      String result;
      if (valid) {
        serializeJson(doc, result);
      } else {
        result = "{\"error\":\"Invalid JSON document in ToolParams\"}";
      }
      return result;
    }
    
    bool isValid() const {
      return valid;
    }
    
  private:
    DynamicJsonDocument doc{2048}; // Adjust size as needed
    bool valid = false;
  };

  // Redefine the tool callback function type - receive JSON string parameters and return the ToolResponse structure
  typedef std::function<ToolResponse(const String&)> ToolCallback; // Change to Receive ToolParams&

  // Callback type definition
  // Output callback: void(const String&)
  typedef void (*OutputCallback)(const String&);
  // Error callback: void(const String&)
  typedef void (*ErrorCallback)(const String&);
  // Connection status callback: void(bool)
  typedef void (*ConnectionCallback)(bool);

  WebSocketMCP();

  /* *
   * Initialize the WebSocket connection
   * @param mcpEndpoint WebSocket server address (ws://host:port/path)
   * @param outputCb output data callback function (equivalent to stdout)
   * @param errorCb error message callback function (equivalent to stderr)
   * @param connCb Connection state change callback function
   * @return Whether the initialization is successful
   *
   * Note: The connection uses the following timeout settings:
   * - PING_INTERVAL: Heartbeat ping interval, default is 45 seconds
   * - DISCONNECT_TIMEOUT: Disconnect timeout, default is 60 seconds
   * - INITIAL_BACKOFF: The initial reconnection wait time is 1 second by default
   * - MAX_BACKOFF: Maximum reconnection waiting time, default is 60 seconds */
  bool begin(const char *mcpEndpoint, ConnectionCallback connCb = nullptr);

  /* *
   * Send data to the WebSocket server (equivalent to stdin)
   * @param message message to send
   * @return Whether the sending is successful */
  bool sendMessage(const String &message);

  /* *
   * Handle WebSocket events and keep connections
   * Need to be called frequently in the main loop */
  void loop();

  /* *
   * Whether it is connected to the server
   * @return Connection status */
  bool isConnected();

  /* *
   * Disconnect */
  void disconnect();

  /* *
   * Set log level and callback function
   * @param level Log level
   * @param logCb log callback function */

  // Tool registration and management methods
  bool registerTool(const String &name, const String &description, const String &inputSchema, ToolCallback callback);
  // Simplify tool registration API
  bool registerSimpleTool(const String &name, const String &description, 
                         const String &paramName, const String &paramDesc, 
                         const String &paramType, ToolCallback callback);
  
  bool unregisterTool(const String &name);
  size_t getToolCount();
  void clearTools();

private:
  WebSocketsClient webSocket;
  ConnectionCallback connectionCallback;

  bool connected;
  unsigned long lastReconnectAttempt;

  // Reconnect settings
  static const int INITIAL_BACKOFF = 1000; // Initial waiting time (milliseconds)
  static const int MAX_BACKOFF = 60000;    // Maximum waiting time (milliseconds)
  static const int PING_INTERVAL = 10000;  // ping sending interval (milliseconds)
  static const int DISCONNECT_TIMEOUT = 60000; // Disconnect timeout (milliseconds)
  int currentBackoff;
  int reconnectAttempt;

  // WebSocket event handling function
  static void webSocketEvent(WStype_t type, uint8_t *payload, size_t length);
  static WebSocketMCP *instance;

  // Reconnect processing
  void handleReconnect();
  void resetReconnectParams();

  // New members
  unsigned long lastPingTime = 0;
  void handleJsonRpcMessage(const String &message);

  // Tool structure definition
  struct Tool {
    String name;           // Tool name
    String description;    // Tool Description
    String inputSchema;    // Tool input schema (JSON format)
    ToolCallback callback; // Tool calls callback functions
  };

  // Tool list
  std::vector<Tool> _tools;

  // Auxiliary methods
  String escapeJsonString(const String &input);
  
  // Format JSON strings, each key-value pair takes up one line
  String formatJsonString(const String &jsonStr);
};

#endif // WEBSOCKET_MCP_H
