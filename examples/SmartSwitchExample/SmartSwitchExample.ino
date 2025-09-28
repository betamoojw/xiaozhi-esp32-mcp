#include <WiFi.h>
#include <WebSocketMCP.h>

// WiFi configuration
const char* ssid = "your-ssid";
const char* password = "your-password";

// MCP server configuration
const char* mcpEndpoint = "ws://your-mcp-server:port/path";

// Create a WebSocketMCP instance
WebSocketMCP mcpClient;

// Smart switch pin definition
const int SWITCH_PINS[] = {1, 2, 3, 4, 5, 6};  // Switch input pins
const int RELAY_PINS[] = {21, 45, 46, 38, 39, 40};  // Relay output pin
bool relayStates[6] = {false, false, false, false, false, false};  // Relay status
unsigned long lastDebounceTime[6] = {0};  // Last anti-shake time
const unsigned long debounceDelay = 50;  // Anti-shake delay (milliseconds)

// Connection status callback function
void onConnectionStatus(bool connected) {
  if (connected) {
    Serial.println("[MCP] Connected to the server");
    // Register tool after successful connection
    registerMcpTools();
  } else {
    Serial.println("[MCP] Disconnect from the server");
  }
}

// Relay control function
void controlRelay(int relayIndex, bool state) {
  if (relayIndex >= 0 && relayIndex < 6) {
    relayStates[relayIndex] = state;
    digitalWrite(RELAY_PINS[relayIndex], state ? HIGH : LOW);
    Serial.printf("[Relay] Control relay %d: %s\n", relayIndex + 1, state ? "open" : "close");
  }
}

// Check switch status
void checkSwitches() {
  for (int i = 0; i < 6; i++) {
    int switchState = digitalRead(SWITCH_PINS[i]);
    unsigned long currentTime = millis();

    // Anti-shake treatment
    if (switchState != relayStates[i] && (currentTime - lastDebounceTime[i] > debounceDelay)) {
      lastDebounceTime[i] = currentTime;
      // The switch is active low
      if (switchState == LOW) {
        controlRelay(i, !relayStates[i]);
      }
    }
  }
}

// Register MCP Tools
void registerMcpTools() {
  // Register Relay Control Tool
  mcpClient.registerTool(
    "relay_control",
    "Control six-channel relay",
    "{\"type\":\"object\",\"properties\":{\"relayIndex\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":6},\"state\":{\"type\":\"boolean\"}},\"required\":[\"relayIndex\",\"state\"]}",
    [](const String& args) {
      DynamicJsonDocument doc(256);
      deserializeJson(doc, args);
      
      int relayIndex = doc["relayIndex"].as<int>() - 1;  // Convert to 0-based index
      bool state = doc["state"].as<bool>();
      
      if (relayIndex >= 0 && relayIndex < 6) {
        controlRelay(relayIndex, state);
        return WebSocketMCP::ToolResponse("{\"success\":true,\"relayIndex\":" + String(relayIndex + 1) + ",\"state\":" + (state ? "true" : "false") + "}");
      } else {
        return WebSocketMCP::ToolResponse("{\"success\":false,\"error\":\"Invalid relay index\"}", true);
      }
    }
  );
  Serial.println("[MCP] Relay Control Tool Registered")l Tool Registered");

  // Register relay status query tool
  mcpClient.registerTool(
    "relay_status",
    "Query the status of six relays",
    "{\"type\":\"object\",\"properties\":{}}",
    [](const String& args) {
      String result = "{\"success\":true,\"relays\":[";
      for (int i = 0; i < 6; i++) {
        if (i > 0) result += ",";
        result += "{\"index\":" + String(i + 1) + ",\"state\":" + (relayStates[i] ? "true" : "false") + "}";
      }
      result += "]}";
      return WebSocketMCP::ToolResponse(result);
    }
  );
  Serial.println("[MCP] Relay status query tool registered");
}

void setup() {
  Serial.begin(115200);

  // Initialize switch pin (input pull-up)
  for (int i = 0; i < 6; i++) {
    pinMode(SWITCH_PINS[i], INPUT_PULLUP);
  }

  // Initialize relay pin (output, initial shutdown)
  for (int i = 0; i < 6; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], LOW);
    relayStates[i] = false;
  }

  // Connect to WiFi
  Serial.print("Connect to WiFi:");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("WiFi is connected");
  Serial.println("IP address:" + WiFi.localIP().toString());

  // Initialize the MCP client
  mcpClient.begin(mcpEndpoint, onConnectionStatus);
}

void loop() {
  // Handle MCP client events
  mcpClient.loop();
  
  // Check switch status
  checkSwitches();
  
  delay(10);
}