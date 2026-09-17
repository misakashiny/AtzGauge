// car_alarm.h -- 车况阈值规则与本地音频告警
//
// 在 ESP-NOW 从表收到主表车况的基础上，判断是否越过安全阈值，越界时：
//   · 串口打印告警（带严重度与实测值）
//   · 通过 Application::Alert() 在屏幕显示 + 播放【本地预录 ogg 音效】
//
// 为什么用本地预录音频而不是云端 TTS：
//   见 开发参考/07-小智AI接入主从架构-技术分析.md 第六节 —— 安全告警要求低延迟
//   (<100ms) 且断网可用。Application::Alert() 播放的是固件内嵌的 ogg，完全本地，
//   不依赖联网，也不需要自建服务端。MCP 工具那条路留给「用户主动问答」。
//
// 三条防误报设计（10Hz 数据 + 会出声的告警，不设防就会刷屏）：
//   1) 迟滞：越过 trigger 才报，回落到 clear 以下才解除，避免在阈值附近抖动
//   2) 重报节流：同一告警持续存在时，最短重报间隔内不再出声
//   3) 陈旧数据门控：主表 2s 没有数据就整体静默（避免拿着过期值报警）

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAR_ALARM_NONE = 0,
    CAR_ALARM_COOLANT_HIGH,   // 水温过高
    CAR_ALARM_OIL_HIGH,       // 机油温度过高
    CAR_ALARM_RPM_HIGH,       // 转速过高
    CAR_ALARM_BAT_LOW,        // 电压过低（发电机/电瓶）
    CAR_ALARM_BAT_HIGH,       // 电压过高（调节器故障）
    CAR_ALARM_COUNT
} car_alarm_id_t;

// 起一个后台监控任务，周期评估阈值规则。
// 需在 espnow_slave_start_async() 之后调用；不阻塞调用者。
void car_alarm_start(void);

// 台架自检：不需要第二块板，也不等真实过热。
//   ① 注入一个已知的「水温 118°C + 油温 130°C + 转速 6800」合成包
//   ② 跑一遍规则评估，核对期望的告警被触发
//   ③ 走一次真实的 Application::Alert() 路径（会出声），证明音频链路可用
//   ④ 复位车况缓存，清掉测试值，避免影响后续真实判断
// 返回 true 表示规则引擎与告警链路都通过。
bool car_alarm_self_test(void);

// 当前最高优先级的活动告警（无则 CAR_ALARM_NONE）
car_alarm_id_t car_alarm_active(void);

// 告警的可读名字（用于日志与将来的 MCP 工具）
const char *car_alarm_name(car_alarm_id_t id);

/**
 * 把屏幕上残留的告警文字收回（告警全部解除时由 evaluate() 自动调用）。
 * 只清字幕，不动设备状态 —— 见 .cc 里的说明（不能用 Application::DismissAlert()）。
 */
void car_alarm_dismiss_alert(void);

#ifdef __cplusplus
}
#endif
