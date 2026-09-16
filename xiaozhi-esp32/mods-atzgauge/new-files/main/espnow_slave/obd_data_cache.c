// obd_data_cache.c -- 车况缓存（从 obd_brz_gauge 移植的裁剪版）
//
// 来源：obd_brz_gauge/main/app_obd_dsp/obd_data_cache.c
// 保留：临界区保护、无效值哨兵、速度平滑、RPM 覆盖层 —— 与上游语义一致
// 裁剪：档位计算（依赖 vehicle_profiles）、里程统计（依赖 nvs_storage）、RS485 刹车温度
//
// 写入侧由 espnow_link.c 收包后调用，读取侧给 AI 阈值判断 / 串口打印 / TTS 播报。

#include "obd_data_cache.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "esp_timer.h"

// 简单全局量，用临界区保护（与上游一致）
static int16_t  s_coolant_temp = -40;
static int16_t  s_oil_temp = -100;
static int16_t  s_intake_temp = -40;
static int16_t  s_load_pct = -1;
static int16_t  s_tps = -1;
static int32_t  s_bat_mv = -1;
static int16_t  s_oil_pressure_x10 = -1;
static int16_t  s_brake_temp_x10 = -1000;
static int16_t  s_boost_x10 = -32768;
static int16_t  s_afr_x100 = -1;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

#define SPEED_SMOOTH_TIME_MS 300   // 速度上升/下降时间常数（ms）
#define FALL_TO_ZERO_MS      500   // 归零时的时间常数（ms）

// 平滑状态（在 setter 侧推进，getter 只读）
static uint16_t s_rpm_smooth = 0;
static uint8_t  s_speed_smooth = 0;
static TickType_t s_speed_last_tick = 0;
static float s_speed_smooth_f = 0.f;

// RPM 覆盖层：多表联动测试时由主表注入模拟转速
static bool     s_rpm_override_en = false;
static uint16_t s_rpm_override_val = 0;

void obd_data_rpm_override_set(bool en, uint16_t val)
{
    portENTER_CRITICAL(&s_mux);
    s_rpm_override_en = en;
    s_rpm_override_val = val;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_rpm(uint16_t rpm)
{
    portENTER_CRITICAL(&s_mux);
    s_rpm_smooth = rpm;   // 源数据已足够干净，不再平滑
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_reset_temp_cache(void)
{
    portENTER_CRITICAL(&s_mux);
    s_coolant_temp = -40;
    s_oil_temp = -100;
    s_intake_temp = -40;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_oil_temp_invalid(void)
{
    portENTER_CRITICAL(&s_mux);
    s_oil_temp = -100;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_speed(uint8_t kmh)
{
    TickType_t now_tick = xTaskGetTickCount();
    uint32_t dt_ms = (uint32_t)((now_tick - s_speed_last_tick) * portTICK_PERIOD_MS);
    if (dt_ms > 1000) dt_ms = 1000;
    s_speed_last_tick = now_tick;

    uint32_t tc = (kmh == 0) ? FALL_TO_ZERO_MS : SPEED_SMOOTH_TIME_MS;
    float alpha = (float)dt_ms / (float)tc;
    if (alpha > 1.0f) alpha = 1.0f;
    s_speed_smooth_f += alpha * ((float)kmh - s_speed_smooth_f);

    uint8_t smoothed = (uint8_t)(s_speed_smooth_f + 0.5f);
    portENTER_CRITICAL(&s_mux);
    s_speed_smooth = smoothed;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_coolant_temp(int16_t temp)
{
    portENTER_CRITICAL(&s_mux);
    s_coolant_temp = temp;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_oil_temp(int16_t temp)
{
    // 与上游一致：越界视为无效，直接忽略（马自达 Mode 22 偶尔会给出垃圾值）
    if (temp < -20 || temp > 150) return;
    portENTER_CRITICAL(&s_mux);
    s_oil_temp = temp;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_intake_temp(int16_t temp)
{
    portENTER_CRITICAL(&s_mux);
    s_intake_temp = temp;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_load_pct(int16_t pct)
{
    portENTER_CRITICAL(&s_mux);
    s_load_pct = pct;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_tps(int16_t pct)
{
    portENTER_CRITICAL(&s_mux);
    s_tps = pct;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_bat_mv(int32_t mv)
{
    portENTER_CRITICAL(&s_mux);
    s_bat_mv = mv;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_oil_pressure_x10(int16_t v)
{
    portENTER_CRITICAL(&s_mux);
    s_oil_pressure_x10 = v;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_boost_x10(int16_t v)
{
    portENTER_CRITICAL(&s_mux);
    s_boost_x10 = v;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_brake_temp_x10(int16_t v)
{
    portENTER_CRITICAL(&s_mux);
    s_brake_temp_x10 = v;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_afr_x100(int16_t v)
{
    portENTER_CRITICAL(&s_mux);
    s_afr_x100 = v;
    portEXIT_CRITICAL(&s_mux);
}

uint16_t obd_data_get_rpm(void)
{
    portENTER_CRITICAL(&s_mux);
    uint16_t v = s_rpm_override_en ? s_rpm_override_val : s_rpm_smooth;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

uint8_t obd_data_get_speed(void)
{
    portENTER_CRITICAL(&s_mux);
    uint8_t v = s_speed_smooth;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

int16_t obd_data_get_coolant_temp(void)
{
    portENTER_CRITICAL(&s_mux);
    int16_t v = s_coolant_temp;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

int16_t obd_data_get_oil_temp(void)
{
    portENTER_CRITICAL(&s_mux);
    int16_t v = s_oil_temp;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

int16_t obd_data_get_intake_temp(void)
{
    portENTER_CRITICAL(&s_mux);
    int16_t v = s_intake_temp;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

int16_t obd_data_get_load_pct(void)
{
    portENTER_CRITICAL(&s_mux);
    int16_t v = s_load_pct;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

int16_t obd_data_get_tps(void)
{
    portENTER_CRITICAL(&s_mux);
    int16_t v = s_tps;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

int32_t obd_data_get_bat_mv(void)
{
    portENTER_CRITICAL(&s_mux);
    int32_t v = s_bat_mv;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

int16_t obd_data_get_oil_pressure_x10(void)
{
    portENTER_CRITICAL(&s_mux);
    int16_t v = s_oil_pressure_x10;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

int16_t obd_data_get_boost_x10(void)
{
    portENTER_CRITICAL(&s_mux);
    int16_t v = s_boost_x10;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

int16_t obd_data_get_brake_temp_x10(void)
{
    portENTER_CRITICAL(&s_mux);
    int16_t v = s_brake_temp_x10;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

int16_t obd_data_get_afr_x100(void)
{
    portENTER_CRITICAL(&s_mux);
    int16_t v = s_afr_x100;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

void obd_data_get_snapshot(obd_data_snapshot_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    out->rpm              = s_rpm_override_en ? s_rpm_override_val : s_rpm_smooth;
    out->speed            = s_speed_smooth;
    out->coolant_temp     = s_coolant_temp;
    out->oil_temp         = s_oil_temp;
    out->intake_temp      = s_intake_temp;
    out->load_pct         = s_load_pct;
    out->tps              = s_tps;
    out->bat_mv           = s_bat_mv;
    out->oil_pressure_x10 = s_oil_pressure_x10;
    out->boost_x10        = s_boost_x10;
    out->brake_temp_x10   = s_brake_temp_x10;
    out->afr_x100         = s_afr_x100;
    portEXIT_CRITICAL(&s_mux);
}

// 把当前车况格式化成一行。无效值统一显示 "--"（沿用上游哨兵约定）。
// 阿特兹为自然吸气，boost / 机油压力 / 刹车温度 恒为无效，故不列入本行。
int obd_data_format_status(char *buf, unsigned buf_len)
{
    if (!buf || buf_len == 0) return 0;
    obd_data_snapshot_t s;
    obd_data_get_snapshot(&s);

    char oil[10], iat[10], load[10], tps[10], afr[10];
    if (s.oil_temp == -100)                 snprintf(oil,  sizeof(oil),  "--");
    else                                    snprintf(oil,  sizeof(oil),  "%d", (int)s.oil_temp);
    if (s.intake_temp == -40)               snprintf(iat,  sizeof(iat),  "--");
    else                                    snprintf(iat,  sizeof(iat),  "%d", (int)s.intake_temp);
    if (s.load_pct < 0)                     snprintf(load, sizeof(load), "--");
    else                                    snprintf(load, sizeof(load), "%d", (int)s.load_pct);
    if (s.tps < 0)                          snprintf(tps,  sizeof(tps),  "--");
    else                                    snprintf(tps,  sizeof(tps),  "%d", (int)s.tps);
    if (s.afr_x100 < 0)                     snprintf(afr,  sizeof(afr),  "--");
    else                                    snprintf(afr,  sizeof(afr),  "%d.%02d", s.afr_x100 / 100, s.afr_x100 % 100);

    char bat[12];
    if (s.bat_mv < 0) snprintf(bat, sizeof(bat), "--");
    else              snprintf(bat, sizeof(bat), "%d.%02d", (int)(s.bat_mv / 1000), (int)((s.bat_mv % 1000) / 10));

    int n = snprintf(buf, buf_len,
                     "RPM %u | SPD %u km/h | COOL %d C | OIL %s C | IAT %s C | LOAD %s%% | TPS %s%% | BAT %s V | AFR %s",
                     (unsigned)s.rpm, (unsigned)s.speed, (int)s.coolant_temp,
                     oil, iat, load, tps, bat, afr);
    return n;
}
