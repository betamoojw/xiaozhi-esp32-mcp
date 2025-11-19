#ifndef WEBSOCKET_MCP_H
#define WEBSOCKET_MCP_H

#include <Arduino.h>
#include <functional>
#include <vector>
#include <ArduinoJson.h> 
#include <WiFiClientSecure.h> // Necessary for TLS/WSS connections on ESP32
#include <Client.h>           // Base class for network sockets

/* *
 * WebSocketMCP Class
 * Encapsulates WebSocket connection and communication with MCP server
 */

// Define the tool response content structure
struct ToolContentItem {
    String type; // Content type, such as "text"
    String text; // Text content
};

// Define tool response structure
class ToolResponse {
public:
    std::vector<ToolContentItem> content; // Response content array
    bool isError; // Is it an error response

    // Constructor: Create a response from a single text content (used for JSON response)
    ToolResponse(const String& textContent, bool error = false) : isError(error) {
        ToolContentItem item;
        item.type = "text";
        item.text = textContent;
        content.push_back(item);
    }

    // Constructor: Create a response from a boolean status and message (used for text response)
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
        // Assume doc has sufficient capacity or is statically allocated correctly.
        DeserializationError error = deserializeJson(doc, json);
        valid = !error;
    }

    // Static factory method from JsonVariantConst
    static ToolParams fromVariant(const JsonVariantConst& variant) {
        String variantJson;
        serializeJson(variant, variantJson);
        return ToolParams(variantJson);
    }

    bool isValid() const { return valid; }
    String getString(const String& key) const { return doc[key].as<String>(); }
    int getInt(const String& key, int defaultValue = 0) const { return doc[key].as<int>() ? doc[key].as<int>() : defaultValue; }
    bool getBool(const String& key, bool defaultValue = false) const { return doc[key].as<bool>() ? doc[key].as<bool>() : defaultValue; }
    float getFloat(const String& key, float defaultValue = 0.0f) const { return doc[key].as<float>() ? doc[key].as<float>() : defaultValue; }

private:
    StaticJsonDocument<512> doc;
    bool valid = false;
};

// Redefine the tool callback function type - receive JSON string parameters and return the ToolResponse structure
typedef std::function<ToolResponse(const String&)> ToolCallback;

// Callback type definition
typedef void (*ConnectionCallback)(bool);

class WebSocketMCP {

public:
    // Original Constructor
    WebSocketMCP();

    // NEW CONSTRUCTOR: Accepts a reference to the configured Client object (for TLS injection).
    WebSocketMCP(Client& client); 

    /* *
    * Initialize the WebSocket connection
    * @param mcpEndpoint WebSocket server address (ws://host:port/path)
    * @param connCb Connection state change callback function
    * @return Whether the initialization is successful
    */
    bool begin(const char *mcpEndpoint, ConnectionCallback connCb = nullptr);

    /* *
    * Send data to the WebSocket server (equivalent to stdin)
    * @param message message to send
    * @return Whether the sending is successful
    */
    bool sendMessage(const String &message);

    /* *
    * Handle WebSocket events and keep connections
    * Must be called frequently in the main loop
    */
    void loop();

    /* *
    * Whether it is connected to the server
    * @return Connection status
    */
    bool isConnected();

    /* *
    * Disconnect
    */
    void disconnect();

    // --- Tool registration and management methods (MCP Protocol) ---

    bool registerTool(const String &name, const String &description, const String &inputSchema, ToolCallback callback);
    bool registerSimpleTool(const String &name, const String &description,
                            const String &paramName, const String &paramDesc,
                            const String &paramType, ToolCallback callback);
    bool unregisterTool(const String &name);
    size_t getToolCount();
    void clearTools();

private:
    // REMOVED: WebSocketsClient webSocket;

    // Pointer to the injected client (WiFiClientSecure)
    // Used by the rewritten network implementation.
    Client* _injectedClient = nullptr; 

    // Internal enumeration for WebSocket State Management (REQUIRED FOR NATIVE)
    enum WsState {
        WS_DISCONNECTED,
        WS_HANDSHAKING,
        WS_CONNECTED
    };
    
    // FIX: New members declared here, consistent with the .cpp initialization lists
    WsState _currentState = WS_DISCONNECTED;
    String _host;
    uint16_t _port;
    String _path;
    bool _isSecure = false;

    ConnectionCallback connectionCallback;

    bool connected;
    unsigned long lastReconnectAttempt;

    // Reconnect settings
    static const int INITIAL_BACKOFF = 1000; 
    static const int MAX_BACKOFF = 60000; 
    static const int PING_INTERVAL = 10000; 
    static const int DISCONNECT_TIMEOUT = 60000; 

    int currentBackoff;
    int reconnectAttempt;

    // FIX: Declarations for the new native WebSocket functions
    bool performHandshake();
    bool sendWebSocketFrame(const String& data, bool isText);
    String receiveWebSocketFrame();
    void processReceivedData();


    // Reconnect processing
    void handleReconnect();
    void resetReconnectParams();

    // Static instance pointer definition (needed for potential static callbacks, although most are member functions now)
    static WebSocketMCP *instance;

    unsigned long lastPingTime = 0;
    void handleJsonRpcMessage(const String &message);

    // Tool structure definition
    struct Tool {
        String name; 
        String description; 
        String inputSchema; 
        ToolCallback callback;
    };

    // Tool list
    std::vector<Tool> _tools;

    // Auxiliary methods
    String escapeJsonString(const String &input);
    String formatJsonString(const String &jsonStr);
};

#endif // WEBSOCKET_MCP_H
