# XiaoZhi MCP Client for ESP32

[![ESP32](https://img.shields.io/badge/ESP32-S3%2FC3%2FP4-000?logo=espressif)](https://www.espressif.com)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Arduino](https://img.shields.io/badge/Arduino-IDE%202.x-blue)](https://www.arduino.cc)
[Xiaozhi Esp32 Server](https://deepwiki.com/xinnan-tech/xiaozhi-esp32-server/1-overview) 

> **Production-ready Model Context Protocol (MCP) client for XiaoZhi AI devices**, enabling ESP32-based hardware (e.g., Grove Vision AI, SenseCap Watcher) to integrate with XiaoZhi’s cloud LLM for real-time perception, reasoning, and action.

![XiaoZhi MCP Flow](https://github.com/djairjr/xiaozhi-esp32-mcp/blob/alpha/docs/mcp-based-graph.jpg)  
*(Simplified MCP interaction — see full sequence in [`docs/mcp-protocol_en.md`](docs/mcp-protocol_en.md))*

---

## ✨ Key Features

- ✅ **Full MCP Protocol Support** (JSON-RPC 2.0 over WebSocket)  
  - `initialize` → `tools/list` → `tools/call` → notifications  
  - Agent-based device activation (`agent_code`)  
  - Automatic token persistence (NVS)
- ✅ **Zero-Copy, Heap-Safe Design**  
  - No `String` in hot paths  
  - All buffers static (stack/member arrays)  
  - PSRAM preserved for future streaming
- ✅ **RFC 6455 WebSocket Compliance**  
  - Client-side masking, ping/pong, close frames  
  - TLS 1.2 with embedded CA (AAA Certificate Services)
- ✅ **FreeRTOS-Ready**  
  - Dedicated network task (Core 0)  
  - Vision/AI task isolation (Core 1)  
  - Watchdog-safe (`vTaskDelay` in loops)
- ✅ **XiaoZhi Official Protocol Alignment**  
  - Based on [`docs/websocket_en.md`](docs/websocket_en.md) and [`docs/mcp-protocol_en.md`](docs/mcp-protocol_en.md)  
  - Headers: `Authorization`, `Protocol-Version`, `Device-Id`, `Client-Id`  
  - Message flow: `hello` → `initialize` → tool invocation

---

## 📦 Installation

### Arduino IDE
1. Download as ZIP: `Code → Download ZIP`
2. **Sketch → Include Library → Add .ZIP Library…**
3. Install **ArduinoJson ≥ 6.21.0** via Library Manager

### PlatformIO
```ini
lib_deps =
    https://github.com/djairjr/xiaozhi-esp32-mcp.git#alpha
    bblanchon/ArduinoJson@^6.21.0
```

---

## 🚀 Quick Start

### 1. Activate your device
Go to [`https://xiaozhi.me/activate?code=yourAgentCodeHere`](https://xiaozhi.me/activate?code=yourAgentCodeHere) and enter your **6-digit agent code**.

### 2. Minimal sketch
```cpp
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "WebSocketMCP.h"

WiFiClientSecure client;
WebSocketMCP mcp(client);

void onMcpConnect(bool connected) {
    if (connected) {
        Serial.println("✅ Connected to XiaoZhi");
        // Register tools AFTER connection (MCP spec)
        mcp.registerTool("report_result", "Detect person wearing glasses",
            R"({"type":"object","properties":{"context_detected":{"type":"boolean"}}})",
            [](const char* args) -> ToolResponse {
                if (strstr(args, "\"context_detected\":true"))
                    digitalWrite(LED_BUILTIN, HIGH);
                return ToolResponse(false, "{\"success\":true}");
            });
    }
}

void setup() {
    Serial.begin(115200);
    WiFi.begin("YOUR_SSID", "YOUR_PASS");
    while (WiFi.status() != WL_CONNECTED) delay(500);

    // ⚡ ONE-LINE ACTIVATION (no token handling!)
    mcp.beginWithAgentCode("yourAgentCodeHere", onMcpConnect);
}

void loop() {
    mcp.loop(); // Handles reconnect, ping, MCP messages
    delay(10);
}
```

---

## 🔧 Protocol Compliance

This library implements **exactly** the flow described in XiaoZhi’s official documentation:

| Step | Action | Documentation Reference |
|------|--------|-------------------------|
| 1 | WebSocket handshake with headers (`Authorization`, `Device-Id`, etc.) | [`docs/websocket_en.md#2-common-request-header`](docs/websocket_en.md#2-common-request-header) |
| 2 | Send `"hello"` with `"features": {"mcp": true}` | [`docs/websocket_en.md#the-device-sends-a-hello-message`](docs/websocket_en.md#the-device-sends-a-hello-message) |
| 3 | Receive server `"hello"` → send `"initialize"` with `agent_code` | [`docs/mcp-protocol_en.md#initialize-mcp-session`](docs/mcp-protocol_en.md#initialize-mcp-session) |
| 4 | Token saved to NVS → auto-reconnect on reboot | [`docs/mcp-protocol_en.md#device-response-timing`](docs/mcp-protocol_en.md#device-response-timing) |
| 5 | Server calls `tools/call` → device executes tool | [`docs/mcp-protocol_en.md#calling-device-tools`](docs/mcp-protocol_en.md#calling-device-tools) |

![MCP Sequence](https://github.com/djairjr/xiaozhi-esp32-mcp/raw/alpha/docs/mcp-sequence.png?raw=true)  
*(Full sequence diagram — see [`docs/mcp-protocol_en.md`](docs/mcp-protocol_en.md))*

---

## 📚 Documentation

All protocol details are sourced from the official XiaoZhi docs:

- [`docs/websocket_en.md`](docs/websocket_en.md) — WebSocket handshake, headers, message types
- [`docs/mcp-protocol_en.md`](docs/mcp-protocol_en.md) — MCP JSON-RPC flow, tool registration, error codes
- [`docs/mqtt-udp_en.md`](docs/mqtt-udp_en.md) — Alternative transport (future support)

> ℹ️ This library implements **only the WebSocket transport** for MCP, as it’s the simplest and most firewall-friendly option.

---

## 🧪 Examples

| Example | Description |
|--------|-------------|
| [`SmartSwitch`](examples/SmartSwitch) | 6-channel relay control via LLM (`tool_call`) |
| [`Basic_Example`](examples/Basic_Example) | Minimal activation + LED control |


## 🛠️ Development

### Testing
1. Enable debug logs: `esp_log_level_set("MCP_WSS", ESP_LOG_DEBUG);`
2. Monitor handshake: watch for `✅ WebSocket handshake successful`
3. Check activation: `✅ Activation successful — token saved`

### Contributing
PRs welcome! Please:
- Keep comments in English
- Avoid heap allocation in hot paths
- Reference XiaoZhi docs for protocol changes

---

## 📜 License

MIT — free for personal and commercial use.

> This project is **not affiliated** with XiaoZhi.ai. It is a community implementation based on publicly available protocol documentation.

---

© 2025 Djair Guilherme — Built for the XiaoZhi ESP32 ecosystem.

