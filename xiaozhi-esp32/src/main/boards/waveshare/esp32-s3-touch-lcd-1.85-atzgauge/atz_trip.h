// atz_trip.h -- 行程统计与峰值保持（「行程」页 + 语音开始/暂停/清零）
//
// 为什么需要：仪表上"当前水温 92°C"没什么信息量，**"本次最高 104°C"**才是判断依据。
// 跑完一段山路/赛道回来，峰值与高转时长是唯一能复盘的数据。
//
// ★ 2026-09-16 用户要求后改成「手动记录」：
//     · 开机默认**不记录**（ATZ_TRIP_RECORD_DEFAULT=0），说「开始记录行程」才开始累加；
//     · 说「暂停记录 / 结束记录」停下；说「行程清零」清空并重新开始；
//     · 停止期间数字保持不动（不是丢失），重新开始会接着累加。
//   为什么默认关：以前是一直在统计，用户看到数字自己变会以为坏了；现在"记录中"三个字
//   明明白白写在「行程」页顶部，用户知道这个数是自己开的。
//
// 数据来源与车况页相同（obd_data_cache 的 10Hz 快照），只要主表在发就能统计。
// 峰值用 RAM 累加、每 ATZ_TRIP_SAVE_S 秒写一次 NVS —— 既扛得住断电，又不会磨损 flash。
#pragma once

#include <stdint.h>

class Display;

/** 建统计定时器（在显示对象 SetupUI() 之后调用一次；内部幂等）。 */
void atz_trip_init(Display* display);

/** 当前统计（任一出参可为 nullptr）。 */
struct atz_trip_stats_t {
    uint16_t max_rpm;
    uint8_t max_speed;
    int16_t max_coolant;
    int16_t max_oil;
    int16_t max_intake;
    int16_t max_load;
    int32_t min_bat_mv;
    uint32_t km_x100;        // 行程里程（0.01 km 为单位；由车速积分估算）
    uint32_t hot_s;          // 转速 ≥ ATZ_TRIP_HOT_RPM 的累计秒数
    uint32_t samples;        // 有效样本数（0 = 还没收到过主表数据）
    uint32_t uptime_s;       // **记录中**的累计时长（秒；暂停期间不增加）
};
void atz_trip_get(atz_trip_stats_t* out);

/** 清零（重新开始一段行程）。写 NVS。**不改变**记录开关。 */
void atz_trip_reset(void);

/** 记录开关（语音/端点/触摸都用它）。掉电不保存：开机永远回到 ATZ_TRIP_RECORD_DEFAULT。 */
void atz_trip_set_recording(bool on);
bool atz_trip_recording(void);

/** 供语音/端点格式化一句话摘要（写进 buf，返回写入长度）。 */
int atz_trip_describe(char* buf, unsigned len);
