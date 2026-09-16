// atz_perf.cc -- 显示层性能统计实现（从 atz_rpm_ring.cc 搬出来，见头文件说明）
#include "atz_perf.h"

#include "display.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

// 直接读显示器的"失效区域"统计（LVGL 私有头；拿不到就退化成只报帧率与耗时）。
// 为什么值得这么干：单帧耗时忽高忽低，只有知道**每次到底重绘了多少像素**，
// 才能判断瓶颈是"画得多"还是"帧太少"。
#if __has_include("display/lv_display_private.h")
#include "display/lv_display_private.h"
#define ATZ_HAVE_DISP_PRIV 1
#endif

#define TAG "AtzPerf"

namespace {

Display* g_display = nullptr;
bool g_inited = false;

volatile uint32_t g_frames = 0;          // 完成的渲染帧数
volatile uint64_t g_busy_us = 0;         // 渲染累计耗时（RENDER_START→RENDER_READY）
volatile uint64_t g_worst_us = 0;        // 最慢一帧
volatile uint64_t g_inv_px_total = 0;    // 累计重绘像素
volatile uint32_t g_inv_px_max = 0;      // 单帧最多重绘像素
volatile uint32_t g_pushes = 0;          // 各 UI 模块上报的"真的改了控件"次数
int64_t g_render_start_us = 0;

void DisplayEventCb(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RENDER_START) {
        g_render_start_us = esp_timer_get_time();
#ifdef ATZ_HAVE_DISP_PRIV
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
            if (px > g_inv_px_max) {
                g_inv_px_max = px;
            }
        }
#endif
    } else if (code == LV_EVENT_RENDER_READY) {
        if (g_render_start_us != 0) {
            const uint64_t dt = (uint64_t)(esp_timer_get_time() - g_render_start_us);
            g_busy_us = g_busy_us + dt;
            if (dt > g_worst_us) {
                g_worst_us = dt;
            }
        }
        g_frames = g_frames + 1;
    }
}

}  // namespace

void atz_perf_init(Display* display) {
    if (display == nullptr || g_inited) {
        return;
    }
    g_inited = true;
    g_display = display;
    lv_display_t* disp = lv_display_get_default();
    if (disp == nullptr) {
        ESP_LOGW(TAG, "no default display, perf counters disabled");
        return;
    }
    lv_display_add_event_cb(disp, DisplayEventCb, LV_EVENT_RENDER_START, nullptr);
    lv_display_add_event_cb(disp, DisplayEventCb, LV_EVENT_RENDER_READY, nullptr);
    ESP_LOGI(TAG, "display perf counters ready (frames / busy / invalidated pixels)");
}

void atz_perf_count_push(void) {
    g_pushes = g_pushes + 1;
}

void atz_perf_read(uint32_t* frames, uint64_t* busy_us, uint32_t* pushes) {
    if (frames != nullptr) *frames = g_frames;
    if (busy_us != nullptr) *busy_us = g_busy_us;
    if (pushes != nullptr) *pushes = g_pushes;
}

void atz_perf_ext(uint64_t* worst_us, uint64_t* inv_px, uint32_t* inv_max) {
    if (worst_us != nullptr) *worst_us = g_worst_us;
    if (inv_px != nullptr) *inv_px = g_inv_px_total;
    if (inv_max != nullptr) *inv_max = g_inv_px_max;
}
