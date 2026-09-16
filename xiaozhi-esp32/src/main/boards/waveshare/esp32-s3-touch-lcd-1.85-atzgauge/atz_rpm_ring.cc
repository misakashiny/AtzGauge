// atz_rpm_ring.cc -- 屏幕外沿的转速环实现（参考 obd_brz_gauge 的 RPM 弧）
//
// ★ 只有**一个环**：整圈 = 底槽，跟着转速点亮的那一段 = 指示弧。
//   参数照抄 obd_brz_gauge 的 ui_ScreenPageRpm.c：
//       lv_arc 340×340 / arc_width 20 / bg_angles 0~360 / rotation 90 / arc_rounded false
//
// 踩坑提醒（写给以后改这里的人）：
//   * 环必须画在**前景**：上游的 container_ 是不透明背景，放在它后面等于看不见；
//   * 环不能吃触摸 → 控件要 clear_flag(LV_OBJ_FLAG_CLICKABLE)，
//     否则点屏幕唤醒对话会被这层挡住；
//   * 本文件的函数可能从 HTTP 任务 / MCP 回调被调用，**必须自己加显示锁**；
//   * 刷新要"抠门"：环的包围盒是 340×340，中间还压着表情图（带缩放的位图，重绘很贵），
//     所以能少重绘一次就少一次 —— 见 ATZ_RPM_RING_STEP 死区。

#include "atz_rpm_ring.h"

#include "application.h"
#include "atz_ui_config.h"
#include "car_alarm.h"
#include "display.h"
#include "espnow_link.h"
#include "obd_data_cache.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

// 注：显示层的帧率/耗时/重绘像素统计已搬到 atz_perf.cc（/perf 端点用），
// 本文件不再需要 LVGL 的私有头。

#define TAG "AtzRpmRing"

#if ATZ_RPM_RING_ENABLE

namespace {

Display* g_display = nullptr;
lv_obj_t* g_arc = nullptr;          // 主环（底槽 + 指示弧）
lv_obj_t* g_bezel = nullptr;        // 最外沿细环（obd_brz_gauge 的 create_ring(page,10)）
lv_timer_t* g_timer = nullptr;
bool g_visible = true;              // 当前是否显示（打开车况页时临时隐藏）
bool g_enabled = true;              // 用户开关（NVS 持久化，语音可控）
uint32_t g_fg = 0xFFFFFF;        // 默认按深色主题（黑底白环）
int g_last_zone = -1;
bool g_last_stale = false;

// 视觉告警状态（轮询 car_alarm_active()，见 ATZ_ALERT_*）
int64_t g_alert_until_us = 0;      // 闪到什么时候
int64_t g_alert_grace_us = 0;      // 告警解除后的余辉截止
bool g_alert_on = false;           // 当前这一拍是不是"红"
car_alarm_id_t g_alert_last_id = CAR_ALARM_NONE;
int g_alert_signalled = -1;        // 上一次已提示的告警 id（-1 = 还没提示过）

// 插值状态（见下面 RingTimerCb 的说明）
int g_disp_value = 0;          // 当前**画在屏上**的值（插值中间值）
int g_target_value = 0;        // 主表最新目标值
int g_pushed_value = -1;       // 上一次真正写进控件的值
int64_t g_next_data_us = 0;    // 下次去车况缓存取数的时刻
bool g_target_stale = true;

// ── 本模块自己的计数（显示层的帧率/耗时统计已搬到 atz_perf.cc）────────────
volatile uint32_t g_ring_calls = 0;         // 环定时器实际被调用的次数
volatile uint32_t g_ring_pushes = 0;        // 其中真的改了控件的次数

constexpr uint32_t kColorWarn = ATZ_RPM_RING_WARN_COLOR;    // 琥珀（接近上限）
constexpr uint32_t kColorAlarm = ATZ_RPM_RING_ALARM_COLOR;  // 红（超限；与 car_alarm / 车况页同色）

/** 指示弧基色：ATZ_RPM_RING_BASE_COLOR 非 0 时用固定色，否则跟随主题文字色。 */
inline uint32_t BaseColor(void) {
#if ATZ_RPM_RING_BASE_COLOR != 0
    return ATZ_RPM_RING_BASE_COLOR;
#else
    return g_fg;
#endif
}

uint32_t ZoneColor(int rpm) {
    if (rpm >= ATZ_RPM_RING_ALARM_RPM) {
        return kColorAlarm;
    }
    if (rpm >= ATZ_RPM_RING_WARN_RPM) {
        return kColorWarn;
    }
    return BaseColor();
}

int ZoneIndex(int rpm) {
    if (rpm >= ATZ_RPM_RING_ALARM_RPM) return 2;
    if (rpm >= ATZ_RPM_RING_WARN_RPM) return 1;
    return 0;
}

/** 底槽色：ATZ_RPM_RING_TRACK_COLOR 非 0 用固定色，否则跟随主题文字色。 */
inline uint32_t TrackColor(void) {
#if ATZ_RPM_RING_TRACK_COLOR != 0
    return ATZ_RPM_RING_TRACK_COLOR;
#else
    return g_fg;
#endif
}

/** 最外沿细环的颜色（obd_brz_gauge 用 UI_COLOR_RING）。 */
inline uint32_t BezelColor(void) {
#if ATZ_RPM_RING_BEZEL_COLOR != 0
    return ATZ_RPM_RING_BEZEL_COLOR;
#else
    return g_fg;
#endif
}

void StyleArc(void) {
    if (g_arc == nullptr) {
        return;
    }
    // 底槽：整圈，同色压暗（浅色主题下不会变成"看不见的白圈"）
    lv_obj_set_style_arc_color(g_arc, lv_color_hex(TrackColor()), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(g_arc, (lv_opa_t)ATZ_RPM_RING_TRACK_OPA, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_arc, ATZ_RPM_RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(g_arc, ATZ_RPM_RING_ROUNDED != 0, LV_PART_MAIN);
    // 指示弧：跟转速走的那一段
    lv_obj_set_style_arc_color(g_arc, lv_color_hex(ZoneColor(0)), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(g_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_arc, ATZ_RPM_RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_arc, ATZ_RPM_RING_ROUNDED != 0, LV_PART_INDICATOR);
    // 不要旋钮
    lv_obj_set_style_bg_opa(g_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_arc, 0, LV_PART_KNOB);
    lv_obj_set_style_border_width(g_arc, 0, LV_PART_KNOB);
}

/**
 * 决定外沿细环这一拍该是什么颜色。优先级：**告警红 > 主题色**。
 * （2026-09-16 用户删掉换挡提示灯：车上已经有自己的换挡灯，屏幕上再闪一个反而分心。）
 */
void ApplyBezelColor(void) {
    if (g_bezel == nullptr) {
        return;
    }
    uint32_t color = BezelColor();
    if (g_alert_on) {
        color = ATZ_ALERT_COLOR;
    }
    lv_obj_set_style_arc_color(g_bezel, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(g_bezel, (lv_opa_t)ATZ_RPM_RING_BEZEL_OPA, LV_PART_MAIN);
}

void StyleBezel(void) {
    if (g_bezel == nullptr) {
        return;
    }
    lv_obj_set_style_arc_color(g_bezel, lv_color_hex(BezelColor()), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(g_bezel, (lv_opa_t)ATZ_RPM_RING_BEZEL_OPA, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_bezel, ATZ_RPM_RING_BEZEL_W, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(g_bezel, false, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(g_bezel, LV_OPA_TRANSP, LV_PART_INDICATOR);   // 只要整圈，不要指示段
    lv_obj_set_style_bg_opa(g_bezel, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_bezel, 0, LV_PART_KNOB);
    lv_obj_set_style_border_width(g_bezel, 0, LV_PART_KNOB);
}

// 只在必要时写 LVGL：静止时一次重绘都不做（死区 ATZ_RPM_RING_STEP）。
//
// ★ 流畅度的关键（实测结论，别再走回头路）：
//   LVGL 的 arc 在改值时走的是 inv_arc_area() —— **只重绘"变化的那一小段弧"**，
//   不是整个包围盒（见 lv_arc.c 的 inv_arc_area / lv_arc_set_end_angle）。
//   所以单帧并不贵；真正让人觉得"卡"的是**更新太稀**：主表 10Hz，一秒只跳 10 下。
//   第一版用 lv_anim 插值，实测只到 27~28fps —— 因为 **LVGL 动画定时器的周期是编译期的
//   LV_DEF_REFR_PERIOD（本项目 33ms）**，动画天生被锁在 ~30fps。
//   所以现在改成**自己做插值**：16ms 一步，每步走剩余差的 1/3（指数逼近），
//   既能跑到 ~60fps，又不会像纯线性插值那样"慢半拍"。
void RingTimerCb(lv_timer_t* timer) {
    // 跑在 LVGL 任务里（锁已经由 lvgl_port 持有），这里不能再取锁
    (void)timer;
    g_ring_calls = g_ring_calls + 1;
    if (g_arc == nullptr || !g_visible || !g_enabled) {
        return;
    }

    // ★ 语音优先：**不是空闲状态**时（连接中/聆听/说话）把插值降到 1/3 频率（≈28fps），
    //   把 CPU 与 PSRAM/总线带宽让给音频链路，避免"对话变卡"。
    //   开车时用户不会同时跟小智聊天，所以这个降载在体验上察觉不到。
    static uint32_t s_tick = 0;
    s_tick++;
    const bool idle_now = (Application::GetInstance().GetDeviceState() == kDeviceStateIdle);
    if (!idle_now && (s_tick % ATZ_RPM_RING_BUSY_DIV) != 0) {
        return;
    }

#if ATZ_ALERT_FLASH_ENABLE
    // ── 视觉告警：轮询告警模块（不改它，只读它的公开状态）────────────────
    // 有活动告警 → 外沿细环按 ATZ_ALERT_FLASH_HZ 在"告警红 ↔ 主题色"之间闪；
    // 告警解除后再补闪 ATZ_ALERT_GRACE_MS，避免一闪而过没注意到。
    if (g_bezel != nullptr && (s_tick % (1000 / ATZ_RPM_RING_TICK_MS / 4)) == 0) {
        const car_alarm_id_t id = car_alarm_active();
        const int64_t now_us = esp_timer_get_time();
        if (id != CAR_ALARM_NONE) {
            if ((int)id != g_alert_signalled) {
                g_alert_signalled = (int)id;
                g_alert_until_us = now_us + (int64_t)ATZ_ALERT_FLASH_MS * 1000;
                ESP_LOGW(TAG, "visual alert: ring flashing (%s) [ring disabled -> log only]",
                         car_alarm_name(id));
            }
            g_alert_grace_us = now_us + (int64_t)ATZ_ALERT_GRACE_MS * 1000;
        }
        const bool active = (now_us < g_alert_until_us) || (now_us < g_alert_grace_us);
        if (!active && g_alert_on) {
            g_alert_on = false;
            g_alert_signalled = -1;
        } else if (active) {
            const int64_t half_period_us = 1000000 / (ATZ_ALERT_FLASH_HZ * 2);
            g_alert_on = ((now_us / half_period_us) % 2) != 0;
        } else {
            g_alert_on = false;
        }
    }

    // 颜色统一在这里落地（告警红 / 主题色 二选一）
    if (g_bezel != nullptr) {
        static uint32_t s_last_color = 0xFFFFFFFF;
        uint32_t want = g_alert_on ? ATZ_ALERT_COLOR : BezelColor();
        if (want != s_last_color) {
            s_last_color = want;
            ApplyBezelColor();
        }
    }
#endif

    // ── 动态帧率：变化慢就没必要每 6ms 插值一次 ──────────────────────────
    //   实测扫掠（转速飞变）83fps 占 ~66% CPU；缓慢漂移时降到 1/3 帧率完全看不出差别。
    if (idle_now) {
        const int gap = (g_target_value > g_disp_value) ? (g_target_value - g_disp_value)
                                                        : (g_disp_value - g_target_value);
        if (gap < ATZ_RPM_RING_SLOW_DELTA && (s_tick % ATZ_RPM_RING_SLOW_DIV) != 0) {
            return;
        }
    }

    const int64_t now = esp_timer_get_time();

    // ── 10Hz：取新目标值 ──────────────────────────────────────────────────
    if (now >= g_next_data_us) {
        g_next_data_us = now + (int64_t)ATZ_RPM_RING_DATA_MS * 1000;
        const bool fresh = espnow_slave_has_data() && espnow_slave_last_rx_age_ms() >= 0 &&
                           espnow_slave_last_rx_age_ms() <= ATZ_UI_STALE_MS;
        int target = 0;
        if (fresh) {
            obd_data_snapshot_t s = {};
            obd_data_get_snapshot(&s);
            target = (int)s.rpm;
            if (target < 0) {
                target = 0;
            }
            if (target > ATZ_RPM_RING_MAX_RPM) {
                target = ATZ_RPM_RING_MAX_RPM;
            }
        }
        if (target > g_target_value + ATZ_RPM_RING_STEP ||
            target + ATZ_RPM_RING_STEP < g_target_value || !fresh != g_target_stale) {
            g_target_value = target;
        }
        // 数据变旧：目标归零，让环自己滑回去（不瞬间跳，观感更像真表）
        if (!fresh) {
            g_target_value = 0;
        }
        const int zone = ZoneIndex(g_target_value);
        if (zone != g_last_zone) {
            g_last_zone = zone;
            lv_obj_set_style_arc_color(g_arc, lv_color_hex(ZoneColor(g_target_value)),
                                       LV_PART_INDICATOR);
        }
        if (!fresh != g_target_stale) {
            g_target_stale = !fresh;
            lv_obj_set_style_arc_opa(g_arc, g_target_stale ? (lv_opa_t)70 : LV_OPA_COVER,
                                     LV_PART_INDICATOR);
        }
    }

    // ── 16ms：插值逼近（指数逼近：每步走剩余差的 1/3） ────────────────────
    if (g_disp_value != g_target_value) {
        int delta = g_target_value - g_disp_value;
        int step = delta / 3;
        if (step == 0) {
            step = (delta > 0) ? 1 : -1;
        }
        g_disp_value += step;
        if ((delta > 0 && g_disp_value > g_target_value) ||
            (delta < 0 && g_disp_value < g_target_value)) {
            g_disp_value = g_target_value;   // 别冲过头
        }
    }
    if (g_disp_value != g_pushed_value) {
        const int diff = (g_disp_value > g_pushed_value) ? (g_disp_value - g_pushed_value)
                                                         : (g_pushed_value - g_disp_value);
        // 变化太小就不写控件（省一次重绘，肉眼也看不出来）
        if (diff >= ATZ_RPM_RING_PUSH_STEP || g_disp_value == g_target_value) {
            g_pushed_value = g_disp_value;
            lv_arc_set_value(g_arc, g_disp_value);
            g_ring_pushes = g_ring_pushes + 1;
        }
    }
}

// LVGL 每帧渲染前后都会发事件，用它统计真实帧率与渲染耗时
void DisplayEventCb(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RENDER_START) {
        g_render_start_us = esp_timer_get_time();
#ifdef ATZ_HAVE_DISP_PRIV
        // 统计这一帧要重绘多少像素（各失效区域之和；已被合并的不重复计）
        lv_display_t* disp = lv_display_get_default();
        if (disp != nullptr) {
            uint32_t px = 0;
            for (uint32_t i = 0; i < disp->inv_p; i++) {
                if (disp->inv_area_joined[i]) {
                    continue;
                }
                px += (uint32_t)lv_area_get_width(&disp->inv_areas[i]) *
                      (uint32_t)lv_area_get_height(&disp->inv_areas[i]);
            }
            g_inv_px_total = g_inv_px_total + px;
            g_inv_px_last = px;
            if (px > g_inv_px_max) {
                g_inv_px_max = px;
            }
        }
#endif
    } else if (code == LV_EVENT_RENDER_READY) {
        if (g_render_start_us != 0) {
            const uint64_t dt = (uint64_t)(esp_timer_get_time() - g_render_start_us);
            g_render_busy_us = g_render_busy_us + dt;
            if (dt > g_render_worst_us) {
                g_render_worst_us = dt;
            }
        }
        g_render_frames = g_render_frames + 1;
    }
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════

void atz_rpm_ring_init(Display* display) {
    if (display == nullptr || g_arc != nullptr) {
        return;   // 幂等：重复调用直接返回（历史教训：绑定非幂等会把主任务刷到看门狗）
    }
    g_display = display;

    DisplayLockGuard lock(display);
    lv_obj_t* screen = lv_screen_active();

    lv_display_t* disp = lv_display_get_default();
    if (disp != nullptr) {
        lv_display_add_event_cb(disp, DisplayEventCb, LV_EVENT_RENDER_START, nullptr);
        lv_display_add_event_cb(disp, DisplayEventCb, LV_EVENT_RENDER_READY, nullptr);
    }

    // ── ② 转速弧（obd_brz_gauge 的 ui_RpmPageArcRpmBack：340×340 / width 20 / 直角）──
    g_arc = lv_arc_create(screen);
    lv_obj_set_size(g_arc, ATZ_RPM_RING_D, ATZ_RPM_RING_D);
    lv_obj_align(g_arc, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(g_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_arc, LV_OBJ_FLAG_SCROLLABLE);
    // ★★ 关键：本环故意比屏幕大（380×380 > 360×360）好让外缘被面板裁掉 —— 但这样一来
    //    屏幕的"可滚动内容"就被撑大了，LVGL 会在屏幕底部画一条**滚动条**（40% 灰、几像素高），
    //    正好压在环面上（用户报的"环上有一个小白条"就是它，实测那一条的合成公式：
    //    α≈0.40 的灰色叠加，切主题整屏重绘后依然存在 → 说明是真控件不是残像）。
    //    解决办法：把环标成 FLOATING —— 浮动对象不参与父级布局，也不计入父级的可滚动范围。
    lv_obj_add_flag(g_arc, LV_OBJ_FLAG_FLOATING);
    lv_arc_set_rotation(g_arc, 90);        // 0° 挪到正上方（12 点）起步
    lv_arc_set_bg_angles(g_arc, 0, 360);   // 整圈
    lv_arc_set_range(g_arc, 0, ATZ_RPM_RING_MAX_RPM);
    lv_arc_set_value(g_arc, 0);
    StyleArc();

    // 双保险：屏幕本身也不允许滚动（上游 UI 正好 360×360，不需要滚动）
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

#if ATZ_RPM_RING_BEZEL
    // ── ① 最外沿细环（obd_brz_gauge 的 ui_helpers_create_ring(page, 10)）────────
    // 直径 360 = 屏幕边（LVGL 弧从半径往内画，所以外缘正好贴死），线宽 10 → 占半径 170~180。
    g_bezel = lv_arc_create(screen);
    lv_obj_set_size(g_bezel, ATZ_RPM_RING_BEZEL_D, ATZ_RPM_RING_BEZEL_D);
    lv_obj_align(g_bezel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(g_bezel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_bezel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_bezel, LV_OBJ_FLAG_FLOATING);   // 别把屏幕撑出滚动条（缺陷 #21）
    lv_arc_set_rotation(g_bezel, 90);
    lv_arc_set_bg_angles(g_bezel, 0, 360);
    lv_arc_set_range(g_bezel, 0, 100);
    lv_arc_set_value(g_bezel, 0);
    StyleBezel();
#endif
    // 画在最前面：上游的 container_ 是不透明底，藏在它后面就看不见了（红区在主环之下）
    lv_obj_move_foreground(g_arc);

    g_timer = lv_timer_create(RingTimerCb, ATZ_RPM_RING_TICK_MS, nullptr);

    // 起始就画 0，并且把插值状态对齐（避免第一次收到数据时从 -1 猛跳）
    g_disp_value = 0;
    g_pushed_value = 0;
    lv_arc_set_value(g_arc, 0);

    // 用户的开关状态（语音改过就写 NVS）：默认 = 编译期 ATZ_RPM_RING_ENABLE
    g_enabled = Settings(ATZ_UI_NVS_NAMESPACE, true).GetInt(ATZ_UI_NVS_KEY_RPM_RING, 1) != 0;
    atz_rpm_ring_set_enabled(g_enabled);

    ESP_LOGI(TAG, "rpm ring ready: d=%d w=%d (outer r=%d = screen edge), full scale=%d rpm "
                  "(warn %d / alarm %d), data %d ms, tick %d ms, push step %d rpm, "
                  "rounded=%d bezel=%d enabled=%d",
             ATZ_RPM_RING_D, ATZ_RPM_RING_W, ATZ_RPM_RING_D / 2, ATZ_RPM_RING_MAX_RPM,
             ATZ_RPM_RING_WARN_RPM, ATZ_RPM_RING_ALARM_RPM, ATZ_RPM_RING_DATA_MS,
             ATZ_RPM_RING_TICK_MS, ATZ_RPM_RING_PUSH_STEP, ATZ_RPM_RING_ROUNDED,
             ATZ_RPM_RING_BEZEL, (int)g_enabled);
}

// 语音开关（写 NVS，重启仍生效）。关闭时把环和红区刻度一起藏掉，并停掉插值。
void atz_rpm_ring_set_enabled(bool on) {
    g_enabled = on;
    if (g_display == nullptr || g_arc == nullptr) {
        return;
    }
    DisplayLockGuard lock(g_display);
    const bool show = on && g_visible;
    if (show) {
        lv_obj_remove_flag(g_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_arc);
#if ATZ_RPM_RING_BEZEL
        if (g_bezel != nullptr) {
            lv_obj_remove_flag(g_bezel, LV_OBJ_FLAG_HIDDEN);
        }
#endif
        RingTimerCb(nullptr);   // 立刻补一帧
    } else {
        lv_obj_add_flag(g_arc, LV_OBJ_FLAG_HIDDEN);
#if ATZ_RPM_RING_BEZEL
        if (g_bezel != nullptr) {
            lv_obj_add_flag(g_bezel, LV_OBJ_FLAG_HIDDEN);
        }
#endif
    }
    ESP_LOGI(TAG, "rpm ring %s", on ? "ON" : "OFF");
}

/** 把当前开关写进 NVS（语音工具与 /ring 端点都调它）。 */
void atz_rpm_ring_save_enabled(bool on) {
    Settings(ATZ_UI_NVS_NAMESPACE, true).SetInt(ATZ_UI_NVS_KEY_RPM_RING, on ? 1 : 0);
    ESP_LOGI(TAG, "rpm ring switch saved to NVS: %d", on ? 1 : 0);
}

bool atz_rpm_ring_enabled(void) {
    return g_enabled;
}

// ── 换挡提示灯：已按用户要求整块删除（2026-09-16）──────────────────────────
// 曾经的 atz_rpm_ring_set_shift_rpm / _shift_rpm / _save_shift_rpm 三个接口、
// NVS 键 shift_rpm、以及细环闪绿灯的逻辑全部移除。车上本来就有换挡灯，
// 屏幕再闪一个既分心又和"告警红"抢同一条细环。要恢复请看 git 历史。

void atz_rpm_ring_set_visible(bool visible) {
    if (g_display == nullptr || g_arc == nullptr) {
        return;
    }
    DisplayLockGuard lock(g_display);
    g_visible = visible;
    if (visible && g_enabled) {
        lv_obj_remove_flag(g_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_arc);
#if ATZ_RPM_RING_BEZEL
        if (g_bezel != nullptr) {
            lv_obj_remove_flag(g_bezel, LV_OBJ_FLAG_HIDDEN);
        }
#endif
        RingTimerCb(nullptr);              // 立刻补一帧，别显示旧值
    } else {
        lv_obj_add_flag(g_arc, LV_OBJ_FLAG_HIDDEN);
#if ATZ_RPM_RING_BEZEL
        if (g_bezel != nullptr) {
            lv_obj_add_flag(g_bezel, LV_OBJ_FLAG_HIDDEN);
        }
#endif
    }
}

bool atz_rpm_ring_visible(void) {
    return g_visible;
}

void atz_rpm_ring_apply_theme(uint32_t fg_rgb) {
    g_fg = fg_rgb;
    if (g_display == nullptr || g_arc == nullptr) {
        return;
    }
    DisplayLockGuard lock(g_display);
    StyleArc();
    StyleBezel();
    g_last_zone = -1;   // 强制下一帧重写指示弧颜色
    RingTimerCb(nullptr);
}

void atz_rpm_ring_refresh(void) {
    if (g_display == nullptr || g_arc == nullptr) {
        return;
    }
    DisplayLockGuard lock(g_display);
    RingTimerCb(nullptr);
}

// 环自己的计数（显示层的帧率/耗时/重绘像素统计在 atz_perf.cc，见 /perf 端点）
void atz_rpm_ring_perf_read(uint32_t* calls, uint32_t* pushes) {
    if (calls != nullptr) {
        *calls = g_ring_calls;
    }
    if (pushes != nullptr) {
        *pushes = g_ring_pushes;
    }
}

#else   // ATZ_RPM_RING_ENABLE == 0：全部变成空函数，编译期就消失

void atz_rpm_ring_init(Display* display) { (void)display; }
void atz_rpm_ring_set_visible(bool visible) { (void)visible; }
bool atz_rpm_ring_visible(void) { return false; }
void atz_rpm_ring_set_enabled(bool on) { (void)on; }
bool atz_rpm_ring_enabled(void) { return false; }
void atz_rpm_ring_save_enabled(bool on) { (void)on; }
void atz_rpm_ring_apply_theme(uint32_t fg_rgb) { (void)fg_rgb; }
void atz_rpm_ring_refresh(void) {}
void atz_rpm_ring_perf_read(uint32_t* calls, uint32_t* pushes) {
    (void)calls; (void)pushes;
}

#endif
