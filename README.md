# xiaozhi-mcp library

This library is the MCP client library for the ESP32 Xiaozhi platform. It connects ESP32 devices to the Xiaozhi platform via the MCP plugin. It supports tool registration and invocation, and can control the device through the Xiaozhi AI speaker.

## Features
- Supports WebSocket and WebSocket Secure (WSS) connections
- Automatic reconnection mechanism to ensure connection stability
- Supports JSON-RPC protocol communication
- Tool registration and call system
- Flexible callback function mechanism
- Support for the ESP32 platform

## Installation Guide

### Method 1: Using the Arduino Library Manager
1. Open the Arduino IDE
2. Click "Tools" -> "Manage Libraries..."
3. Enter "xiaozhi_mcp" in the search box
4. Click "Install"

### Method 2: Manual Installation
1. Download the library's ZIP file
2. Open the Arduino IDE
3. Click "Project" -> "Import Libraries" -> "Add .ZIP Library..."
4. Select the downloaded ZIP file

## Quick Start

The following is a complete example showing how to connect to the MCP server and register the tool:

```cpp
#include <WiFi.h>
#include <WebSocketMCP.h>

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
Serial.println("[MCP] connected to the server");
// Register the tool after successful connection
registerMcpTools();
} else {
Serial.println("[MCP] disconnected from the server");
}
}

// Tool callback function - control the LED
ToolResponse ledControl(const String& params) {
// Parse parameters
ToolParams toolParams(params);
if (!toolParams.isValid()) {
return ToolResponse(true, "Invalid parameter");
}

// Get LED state parameter
String state = toolParams.getString("state");
if (state.isEmpty()) {
return ToolResponse(true, "Missing state parameter");
}

// Control LED
if (state == "on") {
digitalWrite(LED_BUILTIN, HIGH);
return ToolResponse(false, "LED is on");
} else if (state == "off") {
digitalWrite(LED_BUILTIN, LOW);
return ToolResponse(false, "LED is off");
} else {
return ToolResponse(true, "Invalid state value, can only be 'on' or 'off'");
}
}

// Register MCP tool
void registerMcpTools() {
// Register LED control tool
mcpClient.registerTool(
"led_control",
"Control ESP32 onboard LED",
"{\"type\":\"object\",\"properties\":{\"state\":{\"type\":\"string\",\"description\":\"LED state: on/off\"}},\"required\":[\"state\"]}",
ledControl
);

// Register a simple tool (simplified version)
mcpClient.registerSimpleTool(
"say_hello",
"Say hello to the person with the specified name",
"name",
"Name of the person to greet",
"string",
[](const String& params) {
ToolParams p(params);
String name = p.getString("name");
return ToolResponse(false, "Hello, " + name + "!");
}
);
}

void setup() {
Serial.begin(115200);

// Initialize the LED pin
pinMode(LED_BUILTIN, OUTPUT);
digitalWrite(LED_BUILTIN, LOW);

// Connect to WiFi
WiFi.begin(ssid, password);
while (WiFi.status() != WL_CONNECTED) {
delay(500);
Serial.print(".");
}
Serial.println("WiFi connected successfully");

// Initialize MCP client
if (mcpClient.begin(mcpEndpoint, onConnectionStatus)) {
Serial.println("MCP client initialization successful");
} else {
Serial.println("MCP client initialization failed");
}
}

void loop() {
// Handle MCP client events
mcpClient.loop();
delay(10);
}
```

## Instructions

### 1. Connect to the MCP server

1. Configure WiFi network information

2. Set the MCP server endpoint URL

3. Create a `WebSocketMCP` instance

4. Call the `begin()` method to initialize and connect to the server.
5. Call `mcpClient.loop()` in the `loop()` function to handle events.

### 2. Registering a Tool

A tool is a functional interface provided by the device to the MCP server. It can be registered in two ways:

#### Method 1: Complete Registration (with Detailed Parameter Definitions)
```cpp
mcpClient.registerTool(
"tool_name",
"Tool Description",
"{\"type\":\"object\",\"properties\":{\"param1\":{\"type\":\"string\"}},\"required\":[\"param1\"]}",
toolCallback
);
```

#### Method 2: Simplified Registration (for single-parameter tools)
```cpp
mcpClient.registerSimpleTool(
"tool_name",
"Tool Description",
"param_name",
"Parameter Description",
"param_type",

toolCallback
);
```

### 3. Tool Callback Function

The tool callback function receives parameters and returns a response:
```cpp
ToolResponse toolCallback(const String& params) {
// Parse parameters
ToolParams toolParams(params);
if (!toolParams.isValid()) {
return ToolResponse(true, "Invalid parameters");
}

// Process business logic
// ...

// Return result
return ToolResponse(false, "Operation successful");
}
```

### 4. Interacting with the Xiaozhi AI Speaker

1. Ensure the device is successfully connected to the MCP server.
2. Wake up the Xiaozhi AI Speaker and speak a command, for example: "Xiaozhi, turn on the LED on my ESP32."
3. The speaker sends the command to the MCP server.
4. The server calls the corresponding tool registered on the device.
5. The device executes the tool and returns the result.
6. The speaker announces the execution result.

### 5. Debugging Tips

1. Use `Serial.println()` to output debugging information
2. Check if the WiFi connection is working properly
3. Confirm that the MCP server address and port are correct
4. Check the serial port output for error messages
5. Ensure that the tool registration code is called after a successful connection

## API Reference

### WebSocketMCP Class

#### Constructor
```cpp
WebSocketMCP();
```

#### Initialization Method
```cpp
bool begin(const char *mcpEndpoint, ConnectionCallback connCb = nullptr);
```
- `mcpEndpoint`: WebSocket server address (ws://host:port/path)
- `connCb`: Connection status change callback function
- Return value: Initialization success

#### Sending a Message
```cpp
bool sendMessage(const String &message);
```
- `message`: The JSON string to send
- Return value: Whether the send was successful

#### Tool Registration
```cpp
bool registerTool(const String &name, const String &description, const String &inputSchema, ToolCallback callback);
bool registerSimpleTool(const String &name, const String &description, const String &paramName, const String &paramDesc, const String &paramType, ToolCallback callback);
```
- `name`: Tool name
- `description`: Tool description
- `inputSchema`: Input parameter definition in JSON format
- `callback`: Tool callback function
- Return value: Whether the registration was successful

#### Tool Management
```cpp
bool unregisterTool(const String &name);
void clearTools();
size_t getToolCount();
```

#### Connection Status
```cpp
bool isConnected();
void disconnect();
```

### ToolResponse Class

Used to create a tool call response:
```cpp
// Create a text response
ToolResponse(bool isError, const String& message);

// Create a JSON response
ToolResponse(const String& json, bool isError = false);

// Create a response from a JSON object
static ToolResponse fromJson(const JsonObject& json, bool error = false);
```

### ToolParams Class

Used to parse tool parameters:
```cpp
ToolParams(const String& json);
bool isValid() const;
String getString(const String& key) const;
int getInt(const String& key, int defaultValue = 0) const;
bool getBool(const String& key, bool defaultValue = false) const;
float getFloat(const String& key, float defaultValue = 0.0f) const;
```

## Examples

- **BasicExample**: Basic connection and tool registration example
- **SmartSwitchExample**: Smart switch control example

## Related Projects
If you need a more complete smart home solution, we recommend the ha-esp32 project.
- Implements HomeAssistant on the ESP32, integrating with platforms such as Xiaomi, Xiaodu, Tuya, and Tmall Genie.
- Provides an MCP interface, supports large-scale model calls, and enables unified control of home devices.
- Project Address: https://gitee.com/panzuji/ha-esp32

## Version History
- v1.0.0: Initial version, supporting basic WebSocket connections and tool registration.

## License
The xiaozhi-mcp library is licensed under the GNU General Public License v3.0 (GPLv3).

GPLv3 is a copyleft open source software license that allows you to freely use, copy, modify, merge, publish, and distribute the software, subject to the following conditions:

1. Any modified works must also be released under GPLv3.

2. The original copyright and license notices must be retained.

3. If you distribute the software in binary form, you must also provide the corresponding source code.

THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, ARISING OUT OF OR IN ANY WAY CONNECTED WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

For the full text of GPLv3, visit https://www.gnu.org/licenses/gpl-3.0.html