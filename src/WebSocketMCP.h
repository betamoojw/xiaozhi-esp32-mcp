#ifndef WEBSOCKET_MCP_H
#define WEBSOCKET_MCP_H

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h> 
#include <Client.h> 
#include <vector>
#include <functional>
#include <ArduinoJson.h> // Dependency for JSON handling

/* Global memory limits */
// Defines fixed buffer sizes for communication parameters, crucial for stable stack/heap usage on ESP32-S3.
#define MAX_URL_LENGTH 128
#define MAX_PATH_LENGTH 64
#define MAX_MESSAGE_LENGTH 1024 // Max size for incoming JSON/Tool response

// Forward declaration for minimal ToolResponse structure
class ToolResponse {
public:
    // Constructor uses const char* instead of String
    ToolResponse(bool err = false, const char* msg = ""); 
    bool isValid() const { return valid; }
private:
    // Using a fixed static size for simple tool responses (minimizing heap allocation)
    StaticJsonDocument<256> doc; 
    bool valid = false;
    bool error = false; 
};

// Redefine the tool callback function type - input is now C-style string
typedef std::function<ToolResponse(const char*)> ToolCallback;

// Callback type definition
typedef void (*ConnectionCallback)(bool);

class WebSocketMCP {

public:
    // Only accepts Client reference (TLS/Secure)
    WebSocketMCP(Client& client);

    /* *
    * Initialize the WebSocket connection
    * @param mcpEndpoint WebSocket server address (wss://host:port/path)
    * @param connCb Connection state change callback function
    * @return Whether the initialization is successful
    */
    bool begin(const char *mcpEndpoint, ConnectionCallback connCb = nullptr);

    /* *
    * Send data to the WebSocket server (EFFICIENT VERSION: C-style buffer)
    * @param message C-style string buffer to send
    * @param length length of the message
    * @return Whether the sending is successful
    */
    bool sendMessage(const char *message, size_t length);

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

    // All String inputs replaced with const char*
    bool registerTool(const char *name, const char *description, const char *inputSchema, ToolCallback callback);
    bool unregisterTool(const char *name);
    size_t getToolCount();
    void clearTools();

private:

    Client* _injectedClient = nullptr;

    enum WsState {
        WS_DISCONNECTED,
        WS_HANDSHAKING,
        WS_CONNECTED
    };

    WsState _currentState = WS_DISCONNECTED;
    
    // Fixed C-style arrays replace String members, ensuring stable memory layout
    char _host[MAX_URL_LENGTH]; 
    uint16_t _port;
    char _path[MAX_PATH_LENGTH]; 
    bool _isSecure = false;

    ConnectionCallback connectionCallback;

    bool connected = false;
    unsigned long lastReconnectAttempt = 0;

    // Reconnect settings
    static const int INITIAL_BACKOFF;
    static const int MAX_BACKOFF;
    static const int PING_INTERVAL;
    static const int DISCONNECT_TIMEOUT;

    int currentBackoff;
    int reconnectAttempt;

    // Internal message buffer to hold received payload data
    char _receiveBuffer[MAX_MESSAGE_LENGTH]; 

    // Internal functions for framing and protocol
    bool performHandshake();
    bool sendWebSocketFrame(const char* data, size_t length, bool isText); 
    
    // receiveWebSocketFrame now returns the length (size_t)
    size_t receiveWebSocketFrame(); 
    void processReceivedData();

    // Reconnect processing
    void handleReconnect();
    void resetReconnectParams();

    static WebSocketMCP *instance;

    unsigned long lastPingTime = 0;

    // handleJsonRpcMessage now accepts C-style buffer and length
    void handleJsonRpcMessage(const char *message, size_t length);

    // Tool structure definition, updated to use const char*
    struct Tool {
        const char *name;
        const char *description;
        const char *inputSchema;
        ToolCallback callback;
    };

    // Tool list
    std::vector<Tool> _tools;

    // Auxiliary methods 
    int escapeJsonString(const char *input, char *output, size_t max_len);
};

#endif // WEBSOCKET_MCP_H
