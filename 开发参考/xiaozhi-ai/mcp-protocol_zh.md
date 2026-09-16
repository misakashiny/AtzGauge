# MCP (Model Context Protocol) 交互流程（小智AI 官方文档归档）

> 来源：https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-protocol_zh.md
> 原文提示：AI 辅助生成，实现后台服务时请参照代码确认细节

## 概述

MCP 协议用于**后台 API（MCP 客户端）**与 **ESP32 设备（MCP 服务器）**之间的通信，
以便后台发现和调用设备提供的功能（工具）。

消息封装在基础通信协议（WebSocket 或 MQTT）的消息体中，内部结构遵循 **JSON-RPC 2.0**。

## 消息结构

```json
{
  "session_id": "...",
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "method": "...",
    "params": { },
    "id": 1,
    "result": { },
    "error": { }
  }
}
```

## 交互流程

### 1. 连接建立与能力通告

设备启动并连上后台后，发送基础协议的 "hello" 消息：

```json
{
  "type": "hello",
  "version": ...,
  "features": { "mcp": true },
  "transport": "websocket",
  "audio_params": { },
  "session_id": "..."
}
```

### 2. 初始化 MCP 会话

后台发起（首个请求）：

```json
{
  "jsonrpc": "2.0",
  "method": "initialize",
  "params": { "capabilities": { "vision": { "url": "...", "token": "..." } } },
  "id": 1
}
```

设备响应：

```json
{
  "jsonrpc": "2.0", "id": 1,
  "result": {
    "protocolVersion": "2024-11-05",
    "capabilities": { "tools": {} },
    "serverInfo": { "name": "...", "version": "..." }
  }
}
```

### 3. 发现设备工具列表

```json
{ "jsonrpc": "2.0", "method": "tools/list", "params": { "cursor": "" }, "id": 2 }
```

设备响应：

```json
{
  "jsonrpc": "2.0", "id": 2,
  "result": {
    "tools": [
      { "name": "self.get_device_status", "description": "...", "inputSchema": { } },
      { "name": "self.audio_speaker.set_volume", "description": "...", "inputSchema": { } }
    ],
    "nextCursor": "..."
  }
}
```

> 若 `nextCursor` 非空，需带该 cursor 再请求下一页。

### 4. 调用设备工具

```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.audio_speaker.set_volume",
    "arguments": { "volume": 50 }
  },
  "id": 3
}
```

成功响应：

```json
{
  "jsonrpc": "2.0", "id": 3,
  "result": { "content": [ { "type": "text", "text": "true" } ], "isError": false }
}
```

失败响应：

```json
{
  "jsonrpc": "2.0", "id": 3,
  "error": { "code": -32601, "message": "Unknown tool: self.non_existent_tool" }
}
```

### 5. 设备主动发送消息（Notifications）

- 时机：设备内部发生需要通知后台的事件
- 方法名可能是 `notifications/` 开头
- 遵循 JSON-RPC Notification 格式，**没有 id 字段**

```json
{
  "jsonrpc": "2.0",
  "method": "notifications/state_changed",
  "params": { "newState": "idle", "oldState": "connecting" }
}
```

> 原文注：代码示例中没有明确的工具发送此类消息，但 `Application::SendMcpMessage` 的存在
> 暗示设备可能主动发送 MCP 消息。**用之前需看代码确认。**

## 交互序列图

```
Device                        Backend API (Client)
  |                                    |
  |-- Hello Message ("mcp": true) ---->|
  |<------ initialize (request) -------|
  |------- initialize (response) ----->|
  |<------ tools/list (request) -------|
  |------- tools/list (response) ----->|
  |<------ tools/call (request) -------|
  |------- tools/call (response) ----->|
  |--- notifications/... (optional) -->|
```

## ★ 对本项目的意义

| 需求 | 用哪个机制 |
|---|---|
| 用户问"水温多少" | **`tools/call`** 调用我们注册的车况工具 |
| AI 回答时带上实时车况 | 同上，LLM 自行决定是否调用 |
| 设备主动播报告警 | `notifications/`（**存疑，需看代码**）或本地音频（推荐） |

相关代码位置：`main/mcp_server.cc` 的 `McpServer::AddCommonTools` 及各工具实现。
