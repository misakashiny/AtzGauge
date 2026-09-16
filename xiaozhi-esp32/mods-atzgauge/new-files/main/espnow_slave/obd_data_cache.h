// obd_data_cache.h -- 车况缓存（从 obd_brz_gauge 移植的裁剪版）
//
// 来源：obd_brz_gauge/main/app_obd_dsp/obd_data_cache.{c,h}
// 裁剪依据：AI 告警只需要"数值本身"，不需要仪表才关心的功能。
//
// 【刻意裁掉的部分】
//   1. calculate_gear() / enGear / gear 字段
//      → 依赖 vehicle_profiles.h（整套车型库、齿比表、轮胎半径），且档位对 AI 无意义
//   2. vMileageDataStatisticTask() / mileage_timer_cb()
//      → 依赖 nvs_storage.h 的 nvs_stat_update_speed()，属于仪表里程统计
//   3. brake_rs485_status_t
//      → 依赖 RS485 外接刹车温度传感器，阿特兹没有该硬件
//
// 保留的无效值哨兵（与上游一致，勿随意改）：
//   oil_temp          -100     load_pct / tps / bat_mv / oil_pressure_x10 / afr_x100   -1
//   boost_x10         -32768   brake_temp_x10   -1000

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t rpm;
    uint8_t  speed;
    int16_t  coolant_temp;
    int16_t  oil_temp;
    int16_t  intake_temp;
    int16_t  load_pct;
    int16_t  tps;
    int32_t  bat_mv;
    int16_t  oil_pressure_x10;
    int16_t  boost_x10;
    int16_t  brake_temp_x10;
    int16_t  afr_x100;
} obd_data_snapshot_t;

// ---- 写入侧（由 espnow_link 收包后调用）----
void obd_data_set_rpm(uint16_t rpm);
void obd_data_set_speed(uint8_t kmh);
void obd_data_set_coolant_temp(int16_t temp);
void obd_data_set_oil_temp(int16_t temp);          // 实际油温 °C，超出 [-20,150] 会被忽略
void obd_data_set_oil_temp_invalid(void);          // 置为 -100
void obd_data_reset_temp_cache(void);              // 三个温度位复位
void obd_data_set_intake_temp(int16_t temp);
void obd_data_set_load_pct(int16_t pct);           // 发动机负荷 0~100%
void obd_data_set_tps(int16_t pct);                // 节气门开度 0~100%
void obd_data_set_bat_mv(int32_t mv);              // 电瓶电压 mV（12000 = 12.0V）
void obd_data_set_oil_pressure_x10(int16_t v);     // 0.1bar
void obd_data_set_boost_x10(int16_t v);            // 0.1bar，可负
void obd_data_set_brake_temp_x10(int16_t v);       // 0.1°C
void obd_data_set_afr_x100(int16_t v);             // 空燃比 ×100（1470 = 14.7:1）

// RPM 覆盖层：多表联动测试时由主表注入模拟转速；启用后 get 返回覆盖值
void obd_data_rpm_override_set(bool en, uint16_t val);

// ---- 读取侧（AI 逻辑从这里读）----
uint16_t obd_data_get_rpm(void);
uint8_t  obd_data_get_speed(void);
int16_t  obd_data_get_coolant_temp(void);
int16_t  obd_data_get_oil_temp(void);
int16_t  obd_data_get_intake_temp(void);
int16_t  obd_data_get_load_pct(void);
int16_t  obd_data_get_tps(void);
int32_t  obd_data_get_bat_mv(void);
int16_t  obd_data_get_oil_pressure_x10(void);
int16_t  obd_data_get_boost_x10(void);
int16_t  obd_data_get_brake_temp_x10(void);
int16_t  obd_data_get_afr_x100(void);
void     obd_data_get_snapshot(obd_data_snapshot_t *out);

// 便捷：把当前车况格式化成一行（给串口打印 / MCP 工具 / TTS 播报复用）
// 返回写入 buf 的字符数（不含结尾 '\0'）
int obd_data_format_status(char *buf, unsigned buf_len);

#ifdef __cplusplus
}
#endif
