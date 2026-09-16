// car_status_tool.cc -- MCP 工具 self.car.get_status
//
// 让小智能用自然语言回答车况问题（"水温多少"、"机油温度正常吗"、"电压够不够"）。
//
// 为什么用 MCP 工具而不是别的机制（见 开发参考/07-小智AI接入主从架构-技术分析.md 第六节）：
//   MCP 的方向是「云端 LLM -> 设备」，云端通过 tools/list 发现、tools/call 调用。
//   用户提问本身就发生在对话里，所以这条链路天然契合"用户主动问答"，
//   **不需要改服务端、不需要自建服务**。而安全告警走本地预录音频（car_alarm.cc），
//   两者分工明确：一个负责"问什么答什么"，一个负责"出事立刻喊"。
//
// 注意：工具描述是给 LLM 看的提示词，必须写清楚什么时候该调用、数据从哪来、
// 以及数据可能为空 —— 否则模型会在没有数据时编造读数。

#include "car_status_tool.h"

#include "espnow_link.h"
#include "obd_data_cache.h"
#include "car_alarm.h"

#include "mcp_server.h"

#include <stdio.h>
#include <string.h>
#include <string>      // std::string（<string.h> 是 C 的字符串函数，两者都要）

#include "esp_log.h"

static const char *TAG = "car_status_tool";

// 组装给模型读的车况文本。刻意用「字段名 值 单位」的紧凑格式：
// 既省 token，又不易被误读。
static std::string build_status_text()
{
    char line[256];
    obd_data_format_status(line, sizeof(line));

    // 链路新鲜度：模型需要知道这份数据是不是当前值
    // 注意缓冲区要留足：最长的 "NO DATA RECEIVED YET (...)" 分支实测 107 字节，
    // 之前用 char[96] 被编译器（-Werror）拦下。
    char link[192];
    if (!espnow_slave_has_data()) {
        int64_t age = espnow_slave_last_rx_age_ms();
        if (age < 0) {
            // 区分「没收到任何东西」与「收到了别人的帧」：后者说明射频是活的，
            // 问题在信道；前者说明这个信道上没有主表流量。见 espnow_link.h 的说明。
            if (espnow_slave_rx_mismatch_count() > 0) {
                snprintf(link, sizeof(link),
                         "ESP-NOW link: NO VALID DATA, but %u foreign frame(s) were received -- "
                         "the radio works, so the WiFi channel is most likely not the channel the "
                         "instrument master transmits on",
                         (unsigned)espnow_slave_rx_mismatch_count());
            } else {
                snprintf(link, sizeof(link),
                         "ESP-NOW link: NO DATA RECEIVED YET "
                         "(the instrument master is off, out of range, or on another WiFi channel)");
            }
        } else {
            snprintf(link, sizeof(link),
                     "ESP-NOW link: STALE, last packet %.1f s ago "
                     "(values below are not current)", (double)age / 1000.0);
        }
    } else {
        snprintf(link, sizeof(link), "ESP-NOW link: OK, last packet %lld ms ago",
                 (long long)espnow_slave_last_rx_age_ms());
    }

    const char *mname = espnow_slave_master_name();
    car_alarm_id_t alarm = car_alarm_active();

    char out[768];
    snprintf(out, sizeof(out),
             "%s\n"
             "Master: %s\n"
             "Packets received: %u\n"
             "%s\n"
             "Active alarm: %s",
             link,
             (mname && mname[0]) ? mname : "(unknown)",
             (unsigned)espnow_slave_rx_count(),
             line,
             car_alarm_name(alarm));
    return std::string(out);
}

void car_status_tool_register(void)
{
    auto &mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.car.get_status",
        "Get the live vehicle data received from the car instrument master over ESP-NOW.\n"
        "Fields: RPM, speed (km/h), coolant temperature (C), oil temperature (C), intake air "
        "temperature (C), engine load (%), throttle position (%), battery voltage (V), air-fuel "
        "ratio.\n"
        "Use this tool for ANY question about the car's current condition, for example:\n"
        "1. What is the coolant / oil temperature?\n"
        "2. What is the engine RPM or vehicle speed?\n"
        "3. Is the battery voltage normal?\n"
        "4. Is anything wrong with the car right now?\n"
        "IMPORTANT: the response includes an ESP-NOW link status. If it says NO DATA or STALE, "
        "do NOT invent readings -- tell the user the instrument link has no current data. "
        "If it mentions foreign frames received, the WiFi channel is the likely culprit: the "
        "instrument master transmits on WiFi channel 1, so the router must be set to channel 1. "
        "A field shown as '--' means the car does not report it.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            (void)properties;
            std::string s = build_status_text();
            ESP_LOGI(TAG, "get_status -> %s", s.c_str());
            return s;
        });
}
