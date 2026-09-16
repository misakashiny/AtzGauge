# 异步语音通知 notify（小智AI 官方文档归档）

> 来源：https://github.com/78/xiaozhi-esp32/blob/main/docs/notify.md
> 标题：Asynchronous Voice Notifications

## 作用

`notify` 消息让**云端**在设备空闲时播放**单向语音通知**。它不开启对话音频通道，
也**不会启用麦克风上行**。要开始对话仍需用户明确唤醒。

## 消息格式

云端通过设备当前协议的控制连接发送 JSON（MQTT 设备走已有的 MQTT 控制 topic）：

```json
{
  "type": "notify",
  "audio_url": "https://cdn.example.com/audio/task-complete.ogg",
  "subtitles": [
    { "start_ms": 0,    "text": "Your task is complete." },
    { "start_ms": 1800, "text": "The result has been saved." }
  ]
}
```

- **audio_url（必需）**：接受 http:// 和 https://，但**设备不跟随重定向**
- **subtitles（可选）**：按 `start_ms` 排序，播放跨过时间点时更新显示字幕
- 该消息**无确认、无 ID、无状态、无过期字段**；尽力投递，只对在线设备生效

## 音频要求

`audio_url` 必须返回 2xx，内容是**单声道 Ogg Opus** 文件：

- 支持 `Content-Length` 或分块传输
- 增量读取，不会按完整长度分配内存，也不会先下完再播
- 复用现有的有界 HTTP 响应队列 + 2KB Ogg 逻辑包缓冲 + 共享 20 包 Opus 解码队列 + 两帧 PCM 播放队列
- 使用 LiteAudioEngine WakeNet 的设备在播放期间释放 WakeNet 资源，回到 Idle 后重建
- Opus 包时长从 TOC 字节读取（支持 5ms~120ms），**拒绝立体声、结构错误、不完整流、超大包、2.5ms 包**

## 设备行为

设备**仅在 Idle 状态**接受 `notify`，随后：

1. 进入内部 `Notifying` 状态，切到性能模式
2. 关闭正常语音处理和麦克风上行
3. 清空先前播放并排队内置提示音
4. 后台任务发起一次 HTTP GET
5. 增量解复用 Ogg 包，直接送进现有 Opus 解码队列
6. 按 Opus 包到达音频输出的媒体位置显示字幕
7. HTTP 流成功结束且队列播完后才回到 `Idle`

- 唤醒操作会取消 HTTP 拉取、清空队列、走正常唤醒流程
- 网络中断、HTTP 错误、音频非法同样取消播放并回到 Idle
- **设备忙时收到的第二条通知会被忽略**

## xz-mqtt 转发

```json
{
  "method": "forward",
  "clientId": "device-client-id",
  "params": {
    "type": "notify",
    "audio_url": "https://cdn.example.com/audio/task-complete.ogg",
    "subtitles": [{ "start_ms": 0, "text": "Your task is complete." }]
  }
}
```

返回的 `success: true` 只表示消息写入了在线设备连接，**不确认收到或播放**。

## ★ 对本项目的意义（重要）

`notify` 是**云端 → 设备**的方向：云端提供 `audio_url`，设备拉取播放。

所以「设备检测到水温过高 → 主动播报」这条路，用 notify 的话需要：

```
设备发现水温 > 115°C
   ↓ 上报云端（需要自定义协议/服务端）
云端生成 TTS → 上传 Ogg → 下发 notify
   ↓ 设备 HTTP 拉取播放
```

**链路长、依赖联网、延迟 1~3 秒，且需要自己改服务端。**

→ **结论：安全告警不要走 notify，走本地预录音频。**
notify 更适合"云端主动播报的资讯类内容"。
