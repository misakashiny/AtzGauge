# Emoji 心情显示 · 对照表（官方原文整理）

> 来源：官方页面 <https://xiaozhi.dev/docs/development/emotion/>，2026-09-18 抓取。
> 本文件 = 官方那一页的**完整内容** + 一小段"本项目怎么用"。
> ★ 换表情包时直接照本文件的"情绪类型"列命名 PNG 文件（`happy` → `happy.png`）。

---

## 一、概述

大语言模型用**单个 Emoji token** 表达当前心情。这些 Emoji **不会被 TTS 朗读**，
而是作为独立的数据类型返回给客户端（设备端 `Display::SetEmotion()`）。

## 二、数据格式

```json
{
    "type": "llm",
    "text": "😊",
    "emotion": "smile"
}
```

> 注：官方示例里 `emotion` 写的是 `"smile"`，但**对照表里没有 `smile`** ——
> 实际下发的是下表那 21 个类型名。设备端拿不到对应图片时会走回退逻辑（见第四节）。

## 三、Emoji 对照表（21 个，官方原表）

| Emoji | 情绪类型 | 描述 |
| --- | --- | --- |
| 😶 | `neutral` | 中性/平静 |
| 🙂 | `happy` | 开心 |
| 😆 | `laughing` | 大笑 |
| 😂 | `funny` | 有趣 |
| 😔 | `sad` | 悲伤 |
| 😠 | `angry` | 生气 |
| 😭 | `crying` | 哭泣 |
| 😍 | `loving` | 喜爱 |
| 😳 | `embarrassed` | 尴尬 |
| 😲 | `surprised` | 惊讶 |
| 😱 | `shocked` | 震惊 |
| 🤔 | `thinking` | 思考 |
| 😉 | `winking` | 眨眼 |
| 😎 | `cool` | 酷 |
| 😌 | `relaxed` | 放松 |
| 🤤 | `delicious` | 美味 |
| 😘 | `kissy` | 亲吻 |
| 😏 | `confident` | 自信 |
| 😴 | `sleepy` | 困倦 |
| 😜 | `silly` | 傻乎乎 |
| 🙄 | `confused` | 困惑 |

## 四、使用说明（官方原文）

1. 客户端收到包含 `emotion` 字段的响应时，应解析对应情绪类型；
2. 可根据情绪类型调整界面显示或触发动画；
3. TTS 系统应忽略 `text` 字段里的 Emoji 字符。

## 五、注意事项（官方原文）

- Emoji 使用 **UTF-8** 编码；
- 确保客户端具备显示 Emoji 的能力；
- 建议界面上同时显示 Emoji 和对应情绪类型文本。

---

## 六、本项目怎么用（补充）

| 事项 | 我们的做法 |
|---|---|
| 素材来源 | `assets` 分区里的 `noto-color-emoji_128`（21 个 PNG，文件名 = 上表"情绪类型"） |
| 换素材 | `tools\emoji-kit.mjs` 一键推送，规范与步骤见 `../../22-表情包素材与一键推送.md` |
| 未知情绪名 | `atz_ui.cc` 的 `SetEmotion()` 先查表情集，**查不到就换成 `neutral`**（`ATZ_UI_EMOTION_FALLBACK`），避免退化成小字体图标 |
| 系统状态图标 | `wifi_configuring` / `cloud_download` / `upgrade` / `error` 等走 **Material Symbols 字体**，不是彩色表情，**不用准备素材** |
| 车况告警驱动的表情 | `car_alarm.cc` 里每条规则带 `emotion`：`surprised`（超转）/`shocked`（水温、油温）/`sad`（电压低）/`angry`（电压高）—— 所以告警时屏幕表情会跟着变 |

> 我们固件里的实际显示尺寸：素材 128px，按 **125%** 放大到约 160px 显示。
