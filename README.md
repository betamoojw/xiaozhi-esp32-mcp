# 小智 ESP32 MCP 客户端

ESP32 版 WebSocket MCP（Model Context Protocol）客户端，用于连接小智 MCP 服务器并通过工具进行远程控制。

## 功能特性

- WebSocket 连接到 MCP 服务器
- 工具注册和远程调用
- WiFi 连接管理
- LED 状态指示
- 串口命令控制台

## 硬件要求

- ESP32 开发板
- 板载 LED（GPIO 2）

## 依赖库

需要在 Arduino IDE 中安装：

- `ArduinoWebSockets` by Markus Sattler
- `ArduinoJson` by Benoit Blanchon

## 配置

修改 `xiaozhi-esp32-mcp.ino` 中的配置：

```cpp
const char* WIFI_SSID = "你的WiFi名称";
const char* WIFI_PASS = "你的WiFi密码";
const char* MCP_ENDPOINT = "wss://api.xiaozhi.me/mcp/?token=你的访问令牌";
```

## 使用方法

1. 配置WiFi和MCP服务器地址
2. 上传代码到 ESP32
3. 打开串口监控器（115200 波特率）
4. 观察连接状态和工具注册信息

## LED 状态指示

- **快速闪烁（100ms）**：WiFi 未连接
- **慢速闪烁（500ms）**：WiFi 已连接，MCP 未连接
- **常亮**：WiFi 和 MCP 均已连接

## 串口命令

| 命令 | 功能 |
|------|------|
| `help` | 显示帮助信息 |
| `status` | 显示连接状态、IP地址、信号强度 |
| `reconnect` | 重新连接 MCP 服务器 |
| `tools` | 显示已注册工具数量 |
| 其他文本 | 发送到 MCP 服务器 |

## 已实现的工具

### 1. LED 控制工具 (`led_blink`)

控制 ESP32 板载 LED 状态

- 参数：`state` - `"on"` / `"off"` / `"blink"`
- 返回：操作结果和当前状态

### 2. 系统信息工具 (`system-info`)

获取 ESP32 系统信息

- 返回：芯片型号、芯片ID、Flash大小、剩余内存、WiFi状态、IP地址

### 3. 计算器工具 (`calculator`)

简单数学计算（仅支持加减法）

- 参数：`expression` - 如 `"10+5"` 或 `"20-3"`
- 返回：计算结果

## 开发指南

### 添加工具

```cpp
mcpClient.registerTool(
  "工具名称",
  "工具描述", 
  "JSON Schema",
  [](const String& args) {
    // 解析参数并执行操作
    return WebSocketMCP::ToolResponse("结果JSON");
  }
);
```

### 主要回调函数

- `onMcpConnectionChange()`: 连接状态变化
- `registerMcpTools()`: 注册所有工具（连接成功后调用）



## 相关项目推荐

如果您对 ESP32 智能家居控制感兴趣，我们诚挚推荐另一个项目：

### 🏠 [ha-esp32](https://gitee.com/panzuji/ha-esp32) - ESP32 智能家居中枢

这是一个更加完整的 ESP32 智能家居解决方案，具有以下特色功能：

- **多平台对接**：支持小米、小度、涂鸦、天猫精灵等主流智能家居平台
- **HomeAssistant 兼容**：在 ESP32 中实现 HomeAssistant 核心功能
- **MCP 接口**：提供标准 MCP 接口，便于大模型调用和控制
- **统一控制**：一站式管理家庭中的所有智能设备
- **语音控制**：无缝对接各大语音助手平台

如果您希望构建一个功能更全面的智能家居系统，ha-esp32 项目可能更适合您的需求。两个项目可以很好地互补使用，本项目专注于 MCP 通信协议实现，而 ha-esp32 则提供完整的智能家居解决方案。

欢迎访问：[https://gitee.com/panzuji/ha-esp32](https://gitee.com/panzuji/ha-esp32)
