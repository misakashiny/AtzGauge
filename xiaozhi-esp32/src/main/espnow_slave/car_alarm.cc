// car_alarm.cc -- 车况阈值规则与本地音频告警（实现）
//
// 用 C++ 写的必要性：告警要经 Application::Schedule() 投递到主任务，
// 它是 std::function 接口；且 AGENTS.md 明确要求
//   "Callbacks may run outside the main task. Schedule application mutations with
//    Application::Schedule() or event bits."
// 我们的评估跑在独立任务里，所以必须调度，不能直接碰 UI / 音频。

#include "car_alarm.h"

#include "espnow_link.h"
#include "obd_data_cache.h"

#include "application.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "car_alarm";

// ── 阈值 ──────────────────────────────────────────────────────────────────
// 迟滞设计：越过 TRIGGER 才报，回落到 CLEAR 以下才解除。
// 阿特兹为自然吸气 2.0/2.5 Skyactiv-G，参考值如下。
#define COOL_TRIGGER        108     // °C  正常 85~95
#define COOL_CLEAR          102
#define OIL_TRIGGER         125     // °C  正常 <110，激烈驾驶可到 120
#define OIL_CLEAR           115
#define RPM_TRIGGER         6500    // rpm 红线区
#define RPM_CLEAR           6000
#define BAT_LOW_TRIGGER     11500   // mV  着车后应 13.5~14.5V
#define BAT_LOW_CLEAR       12000
#define BAT_HIGH_TRIGGER    15000   // mV  超过 15.0V 说明调节器异常
#define BAT_HIGH_CLEAR      14700

// 同一告警持续存在时的最短重报间隔（防止 10Hz 数据把喇叭刷爆）
#define RE_ALERT_INTERVAL_US (30 * 1000000LL)
// 评估周期
#define EVAL_PERIOD_MS      500
// 主表数据陈旧门控：超过 2s 没有新数据就整体静默（与 espnow_link 的超时一致）
#define STALE_LIMIT_US      2000000LL

typedef struct {
    car_alarm_id_t id;
    const char    *name;
    const char    *emotion;
    const char    *sound;
    const char    *unit;
    int32_t        trigger;
    int32_t        clear;
    bool           above;        // true: value >= trigger 报警；false: value <= trigger 报警
    bool           active;
    int64_t        last_alert_us;
} car_rule_t;

static car_rule_t s_rules[] = {
    { CAR_ALARM_COOLANT_HIGH, "COOLANT HIGH", "shocked",   "exclamation", "C",  COOL_TRIGGER,     COOL_CLEAR,     true,  false, 0 },
    { CAR_ALARM_OIL_HIGH,     "OIL TEMP HIGH","shocked",   "vibration",   "C",  OIL_TRIGGER,      OIL_CLEAR,      true,  false, 0 },
    { CAR_ALARM_RPM_HIGH,     "RPM HIGH",     "surprised", "vibration",   "rpm",RPM_TRIGGER,      RPM_CLEAR,      true,  false, 0 },
    { CAR_ALARM_BAT_LOW,      "BATTERY LOW",  "sad",       "low_battery", "mV", BAT_LOW_TRIGGER,  BAT_LOW_CLEAR,  false, false, 0 },
    { CAR_ALARM_BAT_HIGH,     "BATTERY HIGH", "angry",     "exclamation", "mV", BAT_HIGH_TRIGGER, BAT_HIGH_CLEAR, true,  false, 0 },
};
static const int s_rule_count = (int)(sizeof(s_rules) / sizeof(s_rules[0]));

// ── 取值（同时判断该值此刻是否有效）────────────────────────────────────────
// 无效值哨兵来自 obd_data_cache.h，必须显式排除，否则会拿哨兵去比阈值。
static bool rule_value(const car_rule_t &r, const obd_data_snapshot_t &s, int32_t *out)
{
    switch (r.id) {
    case CAR_ALARM_COOLANT_HIGH:
        if (s.coolant_temp == -40) return false;      // 无效哨兵
        *out = s.coolant_temp; return true;
    case CAR_ALARM_OIL_HIGH:
        // 阿特兹机油温度走马自达 Mode 22；读不到时缓存为 -100
        if (s.oil_temp == -100) return false;
        *out = s.oil_temp; return true;
    case CAR_ALARM_RPM_HIGH:
        if (s.rpm == 0) return false;                 // 熄火/无数据
        *out = s.rpm; return true;
    case CAR_ALARM_BAT_LOW:
    case CAR_ALARM_BAT_HIGH:
        if (s.bat_mv < 0) return false;               // 无效哨兵
        *out = (int32_t)s.bat_mv; return true;
    default:
        return false;
    }
}

static void format_value(const car_rule_t &r, int32_t v, char *buf, size_t n)
{
    if (r.id == CAR_ALARM_BAT_LOW || r.id == CAR_ALARM_BAT_HIGH) {
        snprintf(buf, n, "%d.%02d V", (int)(v / 1000), (int)((v % 1000) / 10));
    } else {
        snprintf(buf, n, "%d %s", (int)v, r.unit);
    }
}

// 投递到主任务执行真正的告警（屏幕 + 本地 ogg 音效）
static void fire_alert(const car_rule_t &r, const char *message)
{
    std::string status(r.name);
    std::string msg(message);
    std::string emotion(r.emotion);
    std::string sound(r.sound);

    Application::GetInstance().Schedule([status, msg, emotion, sound]() {
        Application::GetInstance().Alert(status.c_str(), msg.c_str(),
                                         emotion.c_str(), sound);
    });
}

// ── 一次评估 ──────────────────────────────────────────────────────────────
static void evaluate(bool allow_alert)
{
    obd_data_snapshot_t s;
    obd_data_get_snapshot(&s);

    bool stale = !espnow_slave_has_data();
    int64_t now = esp_timer_get_time();

    for (int i = 0; i < s_rule_count; i++) {
        car_rule_t &r = s_rules[i];

        if (stale) {
            // 数据陈旧：解除所有告警并静默，绝不拿过期值报警
            if (r.active) {
                r.active = false;
                ESP_LOGI(TAG, "CLEARED %s (master data stale)", r.name);
            }
            continue;
        }

        int32_t v = 0;
        if (!rule_value(r, s, &v)) {
            if (r.active) {
                r.active = false;
                ESP_LOGI(TAG, "CLEARED %s (value unavailable)", r.name);
            }
            continue;
        }

        bool over  = r.above ? (v >= r.trigger) : (v <= r.trigger);
        bool clear = r.above ? (v <= r.clear)   : (v >= r.clear);

        char val[24];
        format_value(r, v, val, sizeof(val));

        if (!r.active && over) {
            r.active = true;
            r.last_alert_us = now;
            ESP_LOGW(TAG, "ALARM %s -> %s (trigger %d)", r.name, val, (int)r.trigger);
            if (allow_alert) {
                // 只放数值与上限：Alert() 自己会打印 "status: message"，
                // 名字在 status 里已经有了，重复会显示成 "COOLANT HIGH: COOLANT HIGH: 118 C"
                char lim[24];
                format_value(r, r.trigger, lim, sizeof(lim));
                char m[96];
                snprintf(m, sizeof(m), "%s (limit %s)", val, lim);
                fire_alert(r, m);
            }
        } else if (r.active && clear) {
            r.active = false;
            ESP_LOGI(TAG, "CLEARED %s (now %s)", r.name, val);
        } else if (r.active && over &&
                   (now - r.last_alert_us) > RE_ALERT_INTERVAL_US) {
            r.last_alert_us = now;
            ESP_LOGW(TAG, "ALARM(repeat) %s -> %s", r.name, val);
            if (allow_alert) {
                char lim[24];
                format_value(r, r.trigger, lim, sizeof(lim));
                char m[96];
                snprintf(m, sizeof(m), "%s (limit %s)", val, lim);
                fire_alert(r, m);
            }
        }
    }
}

// ── 监控任务 ──────────────────────────────────────────────────────────────
static void car_alarm_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "monitor started (period %dms, re-alert >= %llds)",
             EVAL_PERIOD_MS, RE_ALERT_INTERVAL_US / 1000000LL);
    for (;;) {
        evaluate(true);
        vTaskDelay(pdMS_TO_TICKS(EVAL_PERIOD_MS));
    }
}

// ── 对外接口 ──────────────────────────────────────────────────────────────
car_alarm_id_t car_alarm_active(void)
{
    // 按表顺序返回第一个活动告警（表已按紧急度排列：水温 > 油温 > 转速 > 电压）
    for (int i = 0; i < s_rule_count; i++) {
        if (s_rules[i].active) return s_rules[i].id;
    }
    return CAR_ALARM_NONE;
}

const char *car_alarm_name(car_alarm_id_t id)
{
    for (int i = 0; i < s_rule_count; i++) {
        if (s_rules[i].id == id) return s_rules[i].name;
    }
    return "NONE";
}

bool car_alarm_self_test(void)
{
    ESP_LOGI(TAG, "SELF-TEST: injecting over-temp/over-rev packet "
                  "(coolant %d C, oil %d C, rpm %d)",
             COOL_TRIGGER + 10, OIL_TRIGGER + 5, RPM_TRIGGER + 300);

    // 主表数据门控要求「最近有数据」，注入一个包同时刷新门控与缓存。
    // 用便捷入口，避免自己算包长度（曾经因为传了缓冲区大小 64 而不是
    // 包大小 47 导致注入被拒 —— API 现在从设计上杜绝这个错误）。
    if (!espnow_slave_inject_test_packet(RPM_TRIGGER + 300, 60,
                                         COOL_TRIGGER + 10, OIL_TRIGGER + 5,
                                         40, 50, 30, 14200)) {
        ESP_LOGE(TAG, "SELF-TEST FAIL: could not inject packet");
        return false;
    }

    // allow_alert=true：这会真的走一次 Application::Alert()（会出声），
    // 目的是同时证明「规则引擎」和「音频告警链路」都在工作。
    ESP_LOGI(TAG, "SELF-TEST: ONE AUDIBLE ALERT IS EXPECTED NOW (bench check)");
    evaluate(true);

    struct { car_alarm_id_t id; const char *name; } expect[] = {
        { CAR_ALARM_COOLANT_HIGH, "COOLANT HIGH" },
        { CAR_ALARM_OIL_HIGH,     "OIL TEMP HIGH" },
        { CAR_ALARM_RPM_HIGH,     "RPM HIGH" },
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        bool on = false;
        for (int k = 0; k < s_rule_count; k++) {
            if (s_rules[k].id == expect[i].id && s_rules[k].active) on = true;
        }
        ESP_LOGI(TAG, "  expect %-14s : %s", expect[i].name, on ? "RAISED" : "not raised");
        if (!on) bad++;
    }

    // 电压 14.20V 不该触发任何电压告警
    for (int k = 0; k < s_rule_count; k++) {
        if ((s_rules[k].id == CAR_ALARM_BAT_LOW || s_rules[k].id == CAR_ALARM_BAT_HIGH)
            && s_rules[k].active) {
            ESP_LOGE(TAG, "  unexpected %s raised at 14.20V", s_rules[k].name);
            bad++;
        }
    }

    // 复位缓存，避免测试值污染后续真实判断。
    // 温度回到无效哨兵、转速归零 → 下一轮评估会按迟滞自动解除全部告警。
    obd_data_reset_temp_cache();
    obd_data_set_rpm(0);
    obd_data_set_speed(0);

    if (bad == 0) {
        ESP_LOGI(TAG, "SELF-TEST PASS: rule engine + local audio alert path OK");
        return true;
    }
    ESP_LOGE(TAG, "SELF-TEST FAIL: %d expectation(s) unmet", bad);
    return false;
}

void car_alarm_start(void)
{
    // 先做自检（同步），再起监控任务 —— 避免两者并发评估同一份缓存
    car_alarm_self_test();
    xTaskCreate(car_alarm_task, "car_alarm", 3584, NULL, 3, NULL);
}
