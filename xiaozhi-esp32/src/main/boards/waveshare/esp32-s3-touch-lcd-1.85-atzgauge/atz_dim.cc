// atz_dim.cc -- 自动调光（夜间 / 长时间无交互时不要一直全亮）
//
// 为什么需要：这块屏在车上是一直亮着的，夜里全亮刺眼、也费电。
// 板子没有环境光传感器，所以用两个"能拿到"的信号来判断：
//   ① **设备状态**：只要在对话（连接/聆听/说话）就保持全亮；
//   ② **空闲时长**：连续 ATZ_DIM_AFTER_S 秒没有交互（也没在对话）就降到 ATZ_DIM_PCT；
//   ③ **本地时间**（联网后 NTP 会有）：落在夜间窗口里再暗一档到 ATZ_DIM_NIGHT_PCT。
//
// 为什么用 lv_timer 而不是自己的任务：判断依据都在 LVGL 侧（状态）+ 背光，
// 1 秒一次的检查用 lv_timer 最省事，也天然跟着 LVGL 任务跑。
//
// ★ 只动**背光亮度**，不切主题。切主题会改 NVS 与整屏配色，副作用大；
//   想夜间自动换深色主题，见 atz_ui_config.h 里 ATZ_DIM_NIGHT_* 的说明。
#include "atz_dim.h"

#include "atz_ui_config.h"
#include "application.h"
#include "board.h"
#include "display.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <ctime>

#define TAG "AtzDim"

#if ATZ_DIM_ENABLE

namespace {

Display* g_display = nullptr;
lv_timer_t* g_timer = nullptr;
int64_t g_last_active_ms = 0;
int g_applied_pct = -1;      // 当前已应用的亮度（-1 = 还没设过）

/** 现在是不是夜间窗口（需要本地时间已同步；没同步就返回 false）。 */
bool InNightWindow(void) {
#if ATZ_DIM_NIGHT_ENABLE
    const time_t now = time(nullptr);
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year < 2025 - 1900) {
        return false;   // 时间还没同步（和上游时钟显示同一个判据）
    }
    const int h = tm_now.tm_hour;
    if (ATZ_DIM_NIGHT_FROM_HOUR <= ATZ_DIM_NIGHT_TO_HOUR) {
        return h >= ATZ_DIM_NIGHT_FROM_HOUR && h < ATZ_DIM_NIGHT_TO_HOUR;
    }
    return h >= ATZ_DIM_NIGHT_FROM_HOUR || h < ATZ_DIM_NIGHT_TO_HOUR;   // 跨零点
#else
    return false;
#endif
}

void Apply(int pct, const char* why) {
    if (pct == g_applied_pct) {
        return;
    }
    auto* bl = Board::GetInstance().GetBacklight();
    if (bl == nullptr) {
        return;
    }
    bl->SetBrightness(pct);
    g_applied_pct = pct;
    ESP_LOGI(TAG, "backlight -> %d%% (%s)", pct, why);
}

void DimTimerCb(lv_timer_t* timer) {
    (void)timer;
    const bool talking = (Application::GetInstance().GetDeviceState() != kDeviceStateIdle);
    if (talking) {
        g_last_active_ms = esp_timer_get_time() / 1000;   // 对话期间算"一直在活动"
        Apply(100, "conversation");
        return;
    }
    const int64_t idle_ms = esp_timer_get_time() / 1000 - g_last_active_ms;
    if (idle_ms < (int64_t)ATZ_DIM_AFTER_S * 1000) {
        Apply(100, "recent activity");
        return;
    }
    if (InNightWindow()) {
        Apply(ATZ_DIM_NIGHT_PCT, "night + idle");
    } else {
        Apply(ATZ_DIM_PCT, "idle");
    }
}

}  // namespace

void atz_dim_init(Display* display) {
    if (display == nullptr || g_timer != nullptr) {
        return;   // 幂等
    }
    g_display = display;
    g_last_active_ms = esp_timer_get_time() / 1000;
    g_applied_pct = 100;
    g_timer = lv_timer_create(DimTimerCb, ATZ_DIM_POLL_MS, nullptr);
    ESP_LOGI(TAG,
             "auto dim ready: idle > %d s -> %d%%, night(%02d:00-%02d:00) -> %d%% "
             "(only backlight, theme untouched)",
             ATZ_DIM_AFTER_S, ATZ_DIM_PCT, ATZ_DIM_NIGHT_FROM_HOUR, ATZ_DIM_NIGHT_TO_HOUR,
             ATZ_DIM_NIGHT_PCT);
}

void atz_dim_kick(void) {
    g_last_active_ms = esp_timer_get_time() / 1000;
    g_applied_pct = -1;   // 强制下一拍重新判定（立刻恢复全亮）
    Apply(100, "user activity");
}

#else

void atz_dim_init(Display* display) { (void)display; }
void atz_dim_kick(void) {}

#endif
