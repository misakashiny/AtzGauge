// atz_trip.cc -- 行程统计与峰值保持实现
//
// 设计要点（写给以后改这里的人）：
//   · 采样 200ms 一次：比主表 10Hz 慢，但**峰值靠的是持续比较**，只要每个采样点都比一次
//     就不会漏掉"持续存在"的峰值（瞬时尖峰可能漏，对水温/油温/转速这类物理量无所谓）。
//   · 里程 = Σ(车速 × 采样间隔)：这是**估算**（没有里程表信号），所以对外一律标注"约"。
//   · 只有"主表数据新鲜"时才统计，否则会把 0 车速、哨兵值（-40/-100）当峰值记进去。
//   · **只有"记录中"才统计**（2026-09-16 用户要求）：开机默认不记录，语音/端点/触摸开始。
//   · 暂停不清零：数字原地停住，重新开始接着累加；「行程清零」才是清空 + 重新开始。
//   · NVS 每 60 秒写一次：断电最多丢 1 分钟，flash 寿命也保住了。
#include "atz_trip.h"

#include "atz_ui_config.h"
#include "display.h"
#include "espnow_link.h"
#include "obd_data_cache.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <cstdio>
#include <cstring>

#define TAG "AtzTrip"

#if ATZ_TRIP_ENABLE

namespace {

Display* g_display = nullptr;
lv_timer_t* g_timer = nullptr;
atz_trip_stats_t g_s = {};
bool g_recording = (ATZ_TRIP_RECORD_DEFAULT != 0);
int64_t g_last_sample_us = 0;
int64_t g_last_save_us = 0;
// 不足 1 秒的零头（别每拍都丢掉，否则时长会慢）
double g_second_acc = 0.0;      // 高转时长的零头
double g_uptime_acc = 0.0;      // 记录时长的零头

// 无效值哨兵（与 obd_data_cache.h 的约定一致）—— 别把哨兵当峰值
constexpr int16_t kCoolantInvalid = -40;
constexpr int16_t kOilInvalid = -100;
constexpr int16_t kIntakeInvalid = -40;

void SaveToNvs(void) {
#if ATZ_TRIP_SAVE_S > 0
    Settings s(ATZ_UI_NVS_NAMESPACE, true);
    s.SetInt("trip_max_rpm", (int32_t)g_s.max_rpm);
    s.SetInt("trip_max_spd", (int32_t)g_s.max_speed);
    s.SetInt("trip_max_cool", (int32_t)g_s.max_coolant);
    s.SetInt("trip_max_oil", (int32_t)g_s.max_oil);
    s.SetInt("trip_max_iat", (int32_t)g_s.max_intake);
    s.SetInt("trip_max_load", (int32_t)g_s.max_load);
    s.SetInt("trip_min_bat", (int32_t)g_s.min_bat_mv);
    s.SetInt("trip_km_x100", (int32_t)g_s.km_x100);
    s.SetInt("trip_hot_s", (int32_t)g_s.hot_s);
    s.SetInt("trip_uptime_s", (int32_t)g_s.uptime_s);
    s.SetInt("trip_samples", (int32_t)g_s.samples);
#endif
}

void LoadFromNvs(void) {
#if ATZ_TRIP_SAVE_S > 0
    Settings s(ATZ_UI_NVS_NAMESPACE, false);
    g_s.max_rpm = (uint16_t)s.GetInt("trip_max_rpm", 0);
    g_s.max_speed = (uint8_t)s.GetInt("trip_max_spd", 0);
    g_s.max_coolant = (int16_t)s.GetInt("trip_max_cool", kCoolantInvalid);
    g_s.max_oil = (int16_t)s.GetInt("trip_max_oil", kOilInvalid);
    g_s.max_intake = (int16_t)s.GetInt("trip_max_iat", kIntakeInvalid);
    g_s.max_load = (int16_t)s.GetInt("trip_max_load", -1);
    g_s.min_bat_mv = (int32_t)s.GetInt("trip_min_bat", 0);
    g_s.km_x100 = (uint32_t)s.GetInt("trip_km_x100", 0);
    g_s.hot_s = (uint32_t)s.GetInt("trip_hot_s", 0);
    g_s.uptime_s = (uint32_t)s.GetInt("trip_uptime_s", 0);
    g_s.samples = (uint32_t)s.GetInt("trip_samples", 0);
    ESP_LOGI(TAG, "trip restored from NVS: max %.4u rpm / %.3u km/h / %d C, %.2f km, %lu s",
             (unsigned)g_s.max_rpm, (unsigned)g_s.max_speed, (int)g_s.max_coolant,
             g_s.km_x100 / 100.0, (unsigned long)g_s.uptime_s);
#endif
}

void TripTimerCb(lv_timer_t* timer) {
    // 跑在 LVGL 任务里（锁已由 lvgl_port 持有），这里不能再取锁
    (void)timer;
    const int64_t now = esp_timer_get_time();
    const double dt_s = (g_last_sample_us == 0) ? 0.0 : (double)(now - g_last_sample_us) / 1e6;
    g_last_sample_us = now;

    // 没在记录：什么都不累加（数字原地停住），但 NVS 该写还得写
    if (!g_recording) {
        return;
    }

    // 只在"主表数据新鲜"时统计：否则 0 车速会被当成正常值、哨兵会被当成温度
    const bool fresh = espnow_slave_has_data() && espnow_slave_last_rx_age_ms() >= 0 &&
                       espnow_slave_last_rx_age_ms() <= ATZ_UI_STALE_MS;
    if (!fresh) {
        return;
    }

    obd_data_snapshot_t s = {};
    obd_data_get_snapshot(&s);

    if (s.rpm > g_s.max_rpm) g_s.max_rpm = s.rpm;
    if (s.speed > g_s.max_speed) g_s.max_speed = s.speed;
    if (s.coolant_temp > kCoolantInvalid && s.coolant_temp > g_s.max_coolant) {
        g_s.max_coolant = s.coolant_temp;
    }
    if (s.oil_temp > kOilInvalid && s.oil_temp > g_s.max_oil) {
        g_s.max_oil = s.oil_temp;
    }
    if (s.intake_temp > kIntakeInvalid && s.intake_temp > g_s.max_intake) {
        g_s.max_intake = s.intake_temp;
    }
    if (s.load_pct >= 0 && s.load_pct > g_s.max_load) {
        g_s.max_load = s.load_pct;
    }
    if (s.bat_mv > 0 && (g_s.min_bat_mv == 0 || s.bat_mv < g_s.min_bat_mv)) {
        g_s.min_bat_mv = s.bat_mv;
    }

    // 里程估算 + 记录时长 + 高转时长：只有采样间隔合理（<1s）才累加，
    // 避免调度抖动（比如 OTA/网络卡一下）把数字放大。
    if (dt_s > 0 && dt_s < 1.0) {
        const double m = (double)s.speed / 3.6 * dt_s;
        g_s.km_x100 += (uint32_t)(m / 10.0 + 0.5);   // 10 m = 0.01 km

        // ★ 记录时长必须是"从按下开始记录起累计"的时间，不能拿 esp_timer/1000 ——
        //   那是**开机**到现在的秒数，用户会看到"刚点开始就显示 35 秒"（真实踩过的坑）。
        g_uptime_acc += dt_s;
        if (g_uptime_acc >= 1.0) {
            const uint32_t whole = (uint32_t)g_uptime_acc;
            g_s.uptime_s += whole;
            g_uptime_acc -= (double)whole;
        }

        if (s.rpm >= ATZ_TRIP_HOT_RPM) {
            g_second_acc += dt_s;
            if (g_second_acc >= 1.0) {
                const uint32_t whole = (uint32_t)g_second_acc;
                g_s.hot_s += whole;
                g_second_acc -= (double)whole;
            }
        }
    }

    g_s.samples++;   // 只统计"记录中且数据新鲜"的样本：暂停/断流时这一行到不了

#if ATZ_TRIP_SAVE_S > 0
    if (now - g_last_save_us >= (int64_t)ATZ_TRIP_SAVE_S * 1000000) {
        g_last_save_us = now;
        SaveToNvs();
    }
#endif
}

}  // namespace

void atz_trip_init(Display* display) {
    if (display == nullptr || g_timer != nullptr) {
        return;   // 幂等
    }
    g_display = display;
    LoadFromNvs();
    g_last_sample_us = esp_timer_get_time();
    g_last_save_us = g_last_sample_us;
    g_timer = lv_timer_create(TripTimerCb, ATZ_TRIP_POLL_MS, nullptr);
    ESP_LOGI(TAG, "trip stats ready: %u ms poll, save every %d s, hot>=%d rpm, recording=%d",
             ATZ_TRIP_POLL_MS, ATZ_TRIP_SAVE_S, ATZ_TRIP_HOT_RPM, (int)g_recording);
}

void atz_trip_get(atz_trip_stats_t* out) {
    if (out != nullptr) {
        *out = g_s;
    }
}

void atz_trip_reset(void) {
    g_s = {};
    g_s.max_coolant = kCoolantInvalid;
    g_s.max_oil = kOilInvalid;
    g_s.max_intake = kIntakeInvalid;
    g_s.max_load = -1;
    g_second_acc = 0.0;
    g_uptime_acc = 0.0;
    g_last_sample_us = esp_timer_get_time();
    SaveToNvs();
    ESP_LOGI(TAG, "trip stats cleared (recording=%d)", (int)g_recording);
}

void atz_trip_set_recording(bool on) {
    if (on == g_recording) {
        return;
    }
    g_recording = on;
    g_last_sample_us = esp_timer_get_time();   // 别把"暂停的那段"算进下一拍
    g_second_acc = 0.0;
    g_uptime_acc = 0.0;
    if (!on) {
        SaveToNvs();      // 停下来就立刻落盘，别等 60 秒
    }
    ESP_LOGI(TAG, "trip recording %s", on ? "STARTED" : "PAUSED");
}

bool atz_trip_recording(void) {
    return g_recording;
}

int atz_trip_describe(char* buf, unsigned len) {
    if (buf == nullptr || len == 0) {
        return 0;
    }
    if (g_s.samples == 0) {
        return snprintf(buf, len, "行程记录还没有数据（%s）。",
                        g_recording ? "正在记录，但还没收到主表数据" : "先说「开始记录行程」");
    }
    return snprintf(buf, len,
                    "%s：约 %.2f 公里，记录 %lu 秒；最高转速 %u rpm，最高车速 %u km/h；"
                    "最高水温 %d°C，最高油温 %d°C，最高进气 %d°C，最高负荷 %d%%；"
                    "最低电压 %.2fV；%d 转以上累计 %lu 秒。",
                    g_recording ? "正在记录" : "已暂停", g_s.km_x100 / 100.0,
                    (unsigned long)g_s.uptime_s, (unsigned)g_s.max_rpm, (unsigned)g_s.max_speed,
                    (int)g_s.max_coolant, (int)g_s.max_oil, (int)g_s.max_intake,
                    (int)g_s.max_load, g_s.min_bat_mv / 1000.0, ATZ_TRIP_HOT_RPM,
                    (unsigned long)g_s.hot_s);
}

#else

void atz_trip_init(Display* display) { (void)display; }
void atz_trip_get(atz_trip_stats_t* out) { (void)out; }
void atz_trip_reset(void) {}
void atz_trip_set_recording(bool on) { (void)on; }
bool atz_trip_recording(void) { return false; }
int atz_trip_describe(char* buf, unsigned len) { (void)buf; (void)len; return 0; }

#endif
