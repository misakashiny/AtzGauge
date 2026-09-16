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
#include "display.h"
#include "espnow_link.h"
#include "obd_data_cache.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

// 直接读显示器的"失效区域"统计（LVGL 私有头，拿不到就退化成只报帧率/耗时）。
// 为什么值得这么干：单帧耗时忽高忽低，只有知道**每次到底重绘了多少像素**才能判断
// 瓶颈是"画得多"还是"帧太少"。
#if __has_include("display/lv_display_private.h")
#include "display/lv_display_private.h"
#define ATZ_HAVE_DISP_PRIV 1
#endif

#define TAG "AtzRpmRing"

#if ATZ_RPM_RING_ENABLE

namespace {

Display* g_display = nullptr;
lv_obj_t* g_arc = nullptr;
lv_timer_t* g_timer = nullptr;
bool g_visible = true;
uint32_t g_fg = 0xFFFFFF;        // 默认按深色主题（黑底白环）
int g_last_zone = -1;
bool g_last_stale = false;

// 插值状态（见下面 RingTimerCb 的说明）
int g_disp_value = 0;          // 当前**画在屏上**的值（插值中间值）
int g_target_value = 0;        // 主表最新目标值
int g_pushed_value = -1;       // 上一次真正写进控件的值
int64_t g_next_data_us = 0;    // 下次去车况缓存取数的时刻
bool g_target_stale = true;

// ── 性能统计（/perf 端点读它）────────────────────────────────────────────
volatile uint32_t g_render_frames = 0;      // 完成的渲染帧数
volatile uint64_t g_render_busy_us = 0;     // 渲染累计耗时（RENDER_START→RENDER_READY）
volatile uint32_t g_ring_calls = 0;         // 环定时器实际被调用的次数
volatile uint32_t g_ring_pushes = 0;        // 其中真的改了控件的次数
volatile uint64_t g_render_worst_us = 0;    // 最慢一帧
volatile uint64_t g_inv_px_total = 0;       // 累计重绘像素
volatile uint32_t g_inv_px_last = 0;        // 上一帧重绘像素
volatile uint32_t g_inv_px_max = 0;         // 单帧最多重绘像素
int64_t g_render_start_us = 0;

constexpr uint32_t kColorWarn = 0xFFA000;   // 琥珀
constexpr uint32_t kColorAlarm = 0xFF3B30;  // 红（与 car_alarm / 车况页一致）

uint32_t ZoneColor(int rpm) {
    if (rpm >= ATZ_RPM_RING_ALARM_RPM) {
        return kColorAlarm;
    }
    if (rpm >= ATZ_RPM_RING_WARN_RPM) {
        return kColorWarn;
    }
    return g_fg;
}

int ZoneIndex(int rpm) {
    if (rpm >= ATZ_RPM_RING_ALARM_RPM) return 2;
    if (rpm >= ATZ_RPM_RING_WARN_RPM) return 1;
    return 0;
}

void StyleArc(void) {
    if (g_arc == nullptr) {
        return;
    }
    // 底槽：整圈，同色压暗（浅色主题下不会变成"看不见的白圈"）
    lv_obj_set_style_arc_color(g_arc, lv_color_hex(g_fg), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(g_arc, (lv_opa_t)ATZ_RPM_RING_TRACK_OPA, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_arc, ATZ_RPM_RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(g_arc, false, LV_PART_MAIN);
    // 指示弧：跟转速走的那一段
    lv_obj_set_style_arc_color(g_arc, lv_color_hex(ZoneColor(0)), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(g_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(g_arc, ATZ_RPM_RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_arc, false, LV_PART_INDICATOR);
    // 不要旋钮
    lv_obj_set_style_bg_opa(g_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_arc, 0, LV_PART_KNOB);
    lv_obj_set_style_border_width(g_arc, 0, LV_PART_KNOB);
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
    if (g_arc == nullptr || !g_visible) {
        return;
    }

    // ★ 语音优先：**不是空闲状态**时（连接中/聆听/说话）把插值降到 1/3 频率（≈28fps），
    //   把 CPU 与 PSRAM/总线带宽让给音频链路，避免"对话变卡"。
    //   开车时用户不会同时跟小智聊天，所以这个降载在体验上察觉不到。
    static uint32_t s_tick = 0;
    s_tick++;
    if ((s_tick % ATZ_RPM_RING_BUSY_DIV) != 0) {
        const bool idle_now = (Application::GetInstance().GetDeviceState() == kDeviceStateIdle);
        if (!idle_now) {
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

    // ── 唯一的一个环 ──────────────────────────────────────────────────────
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

    // 画在最前面：上游的 container_ 是不透明底，藏在它后面就看不见了
    lv_obj_move_foreground(g_arc);

    g_timer = lv_timer_create(RingTimerCb, ATZ_RPM_RING_TICK_MS, nullptr);

    // 起始就画 0，并且把插值状态对齐（避免第一次收到数据时从 -1 猛跳）
    g_disp_value = 0;
    g_pushed_value = 0;
    lv_arc_set_value(g_arc, 0);

    ESP_LOGI(TAG, "rpm ring ready: ONE ring d=%d w=%d (outer edge r=%d = screen edge), "
                  "full scale=%d rpm (warn %d / alarm %d), data %d ms, interpolate %d ms, "
                  "push step %d rpm",
             ATZ_RPM_RING_D, ATZ_RPM_RING_W, ATZ_RPM_RING_D / 2, ATZ_RPM_RING_MAX_RPM,
             ATZ_RPM_RING_WARN_RPM, ATZ_RPM_RING_ALARM_RPM, ATZ_RPM_RING_DATA_MS,
             ATZ_RPM_RING_TICK_MS, ATZ_RPM_RING_PUSH_STEP);
}

void atz_rpm_ring_set_visible(bool visible) {
    if (g_display == nullptr || g_arc == nullptr) {
        return;
    }
    DisplayLockGuard lock(g_display);
    g_visible = visible;
    if (visible) {
        lv_obj_remove_flag(g_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_arc);
        RingTimerCb(nullptr);              // 立刻补一帧，别显示旧值
    } else {
        lv_obj_add_flag(g_arc, LV_OBJ_FLAG_HIDDEN);
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

void atz_rpm_ring_perf(char* buf, size_t len, uint32_t frames, uint64_t busy_us, uint32_t calls,
                       uint32_t pushes) {
    if (buf == nullptr || len == 0) {
        return;
    }
    snprintf(buf, len, "frames=%lu busy=%llu us calls=%lu pushes=%lu", (unsigned long)frames,
             (unsigned long long)busy_us, (unsigned long)calls, (unsigned long)pushes);
}

void atz_rpm_ring_perf_read(uint32_t* frames, uint64_t* busy_us, uint32_t* calls, uint32_t* pushes) {
    if (frames != nullptr) {
        *frames = g_render_frames;
    }
    if (busy_us != nullptr) {
        *busy_us = g_render_busy_us;
    }
    if (calls != nullptr) {
        *calls = g_ring_calls;
    }
    if (pushes != nullptr) {
        *pushes = g_ring_pushes;
    }
}

void atz_rpm_ring_perf_ext(uint64_t* worst_us, uint64_t* inv_px, uint32_t* inv_max) {
    if (worst_us != nullptr) {
        *worst_us = g_render_worst_us;
    }
    if (inv_px != nullptr) {
        *inv_px = g_inv_px_total;
    }
    if (inv_max != nullptr) {
        *inv_max = g_inv_px_max;
    }
}

#else   // ATZ_RPM_RING_ENABLE == 0：全部变成空函数，编译期就消失

void atz_rpm_ring_init(Display* display) { (void)display; }
void atz_rpm_ring_set_visible(bool visible) { (void)visible; }
bool atz_rpm_ring_visible(void) { return false; }
void atz_rpm_ring_apply_theme(uint32_t fg_rgb) { (void)fg_rgb; }
void atz_rpm_ring_refresh(void) {}
void atz_rpm_ring_perf(char* buf, size_t len, uint32_t frames, uint64_t busy_us, uint32_t calls,
                       uint32_t pushes) {
    (void)buf; (void)len; (void)frames; (void)busy_us; (void)calls; (void)pushes;
}
void atz_rpm_ring_perf_read(uint32_t* frames, uint64_t* busy_us, uint32_t* calls, uint32_t* pushes) {
    (void)frames; (void)busy_us; (void)calls; (void)pushes;
}
void atz_rpm_ring_perf_ext(uint64_t* worst_us, uint64_t* inv_px, uint32_t* inv_max) {
    (void)worst_us; (void)inv_px; (void)inv_max;
}

#endif
