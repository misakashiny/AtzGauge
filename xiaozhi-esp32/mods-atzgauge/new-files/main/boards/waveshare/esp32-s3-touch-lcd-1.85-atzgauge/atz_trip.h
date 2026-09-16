// atz_trip.h -- 行程统计与峰值保持
//
// 为什么需要：仪表上"当前水温 92°C"没什么信息量，**"本次最高 104°C"**才是判断依据。
// 跑完一段山路/赛道回来，峰值与超转时长是唯一能复盘的数据。
//
// 数据来源与转速环相同（obd_data_cache 的 10Hz 快照），所以只要主表在发就能统计。
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
    uint32_t above_shift_s;  // 转速 ≥ 换挡阈值的累计秒数
    uint32_t samples;        // 有效样本数（0 = 本次开机还没收到过主表数据）
    uint32_t uptime_s;       // 统计持续时长（秒）
};
void atz_trip_get(atz_trip_stats_t* out);

/** 清零（重新开始一段行程）。写 NVS。 */
void atz_trip_reset(void);

/** 供语音/端点格式化一句话摘要（写进 buf，返回写入长度）。 */
int atz_trip_describe(char* buf, unsigned len);
