#include <WiFi.h>
#include <WebSocketMCP.h>

#define LED_BUILTIN 2

// WiFi configuration
const char* ssid = "your-ssid";
const char* password = "your-password";

// MCP server configuration
const char* mcpEndpoint = "ws://your-mcp-server:port/path";

// Create a WebSocketMCP instance
WebSocketMCP mcpClient;

// Connection status callback function
void onConnectionStatus(bool connected) {
  if (connected) {
    Serial.println("[MCP] Connected to the server") to the server");
    // Register tool after successful connection
    registerMcpTools();
  } else {
    Serial.println("[MCP] Disconnect from the server") from the server");
  }
}

// Register MCP Tools
void registerMcpTools() {
  // Register a simple LED control tool
  mcpClient.registerTool(
    "led_blink",
    "Control ESP32 onboard LEDs",
    "{\"type\":\"object\",\"properties\":{\"state\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"blink\"]}},\"required\":[\"state\"]}",
    [](const String& args) {
      DynamicJsonDocument doc(256);
      deserializeJson(doc, args);
      String state = doc["state"].as<String>();
      
      if (state == "on") {
        digitalWrite(LED_BUILTIN, HIGH);
      } else if (state == "off") {
        digitalWrite(LED_BUILTIN, LOW);
      } else if (state == "blink") {
        for (int i = 0; i < 5; i++) {
          digitalWrite(LED_BUILTIN, HIGH);
          delay(200);
          digitalWrite(LED_BUILTIN, LOW);
          delay(200);
        }
      }
      
      return WebSocketMCP::ToolResponse("{\"success\":true,\"state\":\"" + state + "\"}");
    }
  );
  Serial.println("[MCP] LED Control Tool Registered")Tool Registered");
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // Connect to WiFi
  Serial.print("Connect to WiFi:")WiFi:");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("WiFi is connected")onnected");
  Serial.println("IP address:" + WiFi.localIP().toString());

  // Initialize the MCP client
  mcpClient.begin(mcpEndpoint, onConnectionStatus);
}

void loop() {
  // Handle MCP client events
  mcpClient.loop();
  
  // Other codes...
  delay(10);
}