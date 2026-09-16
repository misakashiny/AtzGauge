// atz_arc_text.cc -- 字幕「逐字贴弧」实现
//
// ── 几何 ────────────────────────────────────────────────────────────────────
// 屏幕中心 (180,180)，文字沿半径 R=ATZ_ARC_TEXT_RADIUS 的圆弧排布，弧的中心方向是正下方。
// 第 i 个字的角度 φᵢ（从正下方算起，向右为正）：
//
//     位置   x = cx + R·sin(φ)          y = cy + R·cos(φ)
//     旋转   transform_rotation = −φ    （LVGL 正角度是顺时针，取负 → 字顶朝圆心，成"笑脸"）
//
// 每个字占的弧长 = 它自己的字宽（用 lv_txt_get_size 量），换算成角度就是 w/R（弧度）。
// 这样中英文混排也不会挤在一起。
//
// ── 为什么不改上游 ──────────────────────────────────────────────────────────
// 上游的 chat_message_label_ / bottom_bar_ 一个字节都不动，只是把它们藏起来，
// 我们自己在同一个屏幕上叠一层。想退回直线字幕：ATZ_ARC_TEXT_ENABLE=0 重新编译，
// 或者运行时 atz_arc_text_set_enabled(false)。
//
// ── 踩坑 ────────────────────────────────────────────────────────────────────
//   * 单字要设成 LV_SIZE_CONTENT（否则默认整宽，align 出来位置全错）；
//   * 旋转是**渲染期变换**，不改变控件自身的盒模型 —— align 用的是未旋转的盒子，正好，
//     因为我们就是要"把字的中心放在弧上、再原地旋转"；
//   * lv_label_set_text 之后要先 lv_obj_update_layout 再量尺寸，否则量到的是旧字宽；
//   * 本文件的函数可能从 HTTP/MCP 任务调用 → 必须自己加显示锁（用板级的 atz_ui_copy_* 接口取文本）。

#include "atz_arc_text.h"

#include "atz_ui.h"
#include "atz_ui_config.h"
#include "display.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <lvgl.h>

#include <cmath>
#include <cstring>

#define TAG "AtzArcText"

#if ATZ_ARC_TEXT_ENABLE

namespace {

Display* g_display = nullptr;
lv_obj_t* g_layer = nullptr;                 // 整屏透明层
lv_obj_t* g_chars[ATZ_ARC_TEXT_MAX_CHARS] = {};
int g_char_count = 0;
lv_timer_t* g_timer = nullptr;
bool g_enabled = true;
char g_last_text[160] = {0};

// 把一段 UTF-8 拆成单个字符（中文 3 字节 / ASCII 1 字节都支持）。
// 返回下一个字符的起始下标。
int NextUtf8Char(const char* s, int start, int len, char* out, size_t out_len) {
    int i = start;
    if (i >= len) {
        return len;
    }
    const unsigned char c = (unsigned char)s[i];
    int n = 1;
    if ((c & 0x80) == 0x00) {
        n = 1;
    } else if ((c & 0xE0) == 0xC0) {
        n = 2;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
    }
    if (i + n > len) {
        n = 1;
    }
    const size_t copy = (size_t)n < out_len - 1 ? (size_t)n : out_len - 1;
    memcpy(out, s + i, copy);
    out[copy] = '\0';
    return i + n;
}

// 量一个字在当前字体下的宽度（像素）
int CharWidth(const char* ch, const lv_font_t* font) {
    lv_point_t size = {0, 0};
    lv_text_get_size(&size, ch, font, 0, 0, 1000, LV_TEXT_FLAG_NONE);
    return (int)size.x > 0 ? (int)size.x : 8;
}

void HideAllChars(void) {
    for (int i = 0; i < ATZ_ARC_TEXT_MAX_CHARS; i++) {
        if (g_chars[i] != nullptr) {
            lv_obj_add_flag(g_chars[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// 按文本重排。要求调用者已持有显示锁。
void LayoutText(const char* text) {
    if (g_layer == nullptr) {
        return;
    }
    HideAllChars();
    g_char_count = 0;

    if (text == nullptr || text[0] == '\0') {
        return;
    }

    const int len = (int)strlen(text);
    // 先按 UTF-8 拆字（最多 ATZ_ARC_TEXT_MAX_CHARS 个，超了用省略号收尾）
    char chars[ATZ_ARC_TEXT_MAX_CHARS][8] = {};
    int widths[ATZ_ARC_TEXT_MAX_CHARS] = {};
    int n = 0;
    int pos = 0;
    bool truncated = false;
    while (pos < len && n < ATZ_ARC_TEXT_MAX_CHARS) {
        pos = NextUtf8Char(text, pos, len, chars[n], sizeof(chars[n]));
        if (chars[n][0] == '\n' || chars[n][0] == '\r') {
            continue;   // 换行符直接跳过
        }
        n++;
    }
    if (pos < len && n >= ATZ_ARC_TEXT_MAX_CHARS) {
        truncated = true;
        n = ATZ_ARC_TEXT_MAX_CHARS - 1;
        snprintf(chars[n], sizeof(chars[n]), "…");
    }
    if (n == 0) {
        return;
    }

    // ★★ 字体**绝对不要缓存到控件上** ★★
    // 这个项目的字体是"资源"：启动 ~5 秒后 `Assets: Refreshing display theme...` 会重建主题
    // 和字体对象，换主题时也会。之前这里对每个字控件调了 lv_obj_set_style_text_font()，
    // 于是控件里存的是一个**会失效的指针** → 下一次排版/渲染就崩在
    // lv_font_get_glyph_width()（实测：InstrFetchProhibited，开机 5 秒左右必崩）。
    // 正确做法：**不设字体，让它从父级继承**（屏幕的字体由上游在每次换主题时更新），
    // 需要量字宽时**当场**从当前屏幕取一次。
    const lv_font_t* font = lv_obj_get_style_text_font(lv_screen_active(), LV_PART_MAIN);
    if (font == nullptr) {
        font = lv_obj_get_style_text_font(g_layer, LV_PART_MAIN);
    }
    if (font == nullptr) {
        ESP_LOGE(TAG, "no font available, skip arc layout");
        return;
    }
    uint32_t color = ATZ_ARC_TEXT_COLOR;
    if (color == 0) {
        auto* theme = LvglThemeManager::GetInstance().GetTheme("light");
        // 用当前主题文字色；取不到就退回白色（深色主题下正确）
        Display* d = g_display;
        if (d != nullptr && d->GetTheme() != nullptr) {
            auto* t = static_cast<LvglTheme*>(d->GetTheme());
            color = lv_color_to_u32(t->text_color());
        } else if (theme != nullptr) {
            color = lv_color_to_u32(theme->text_color());
        } else {
            color = 0xFFFFFF;
        }
    }

    // 量宽度
    int total_w = 0;
    for (int i = 0; i < n; i++) {
        widths[i] = CharWidth(chars[i], font);
        total_w += widths[i];
    }

    const double R = ATZ_ARC_TEXT_RADIUS;
    // 总角度：所有字宽累加 / 半径
    const double total_rad = (double)total_w / R;
    double phi = -total_rad / 2.0;   // 从左往右排，起点在左侧

    const int cx = LV_HOR_RES / 2;
    const int cy = LV_VER_RES / 2;
    const double center_rad = (double)ATZ_ARC_TEXT_ANGLE * M_PI / 180.0;

    for (int i = 0; i < n; i++) {
        const double half = (double)widths[i] / R / 2.0;
        phi += half;                       // 走到这个字的中心
        const double a = phi + center_rad; // 相对正下方的角度
        const int x = cx + (int)lround(R * sin(a));
        const int y = cy + (int)lround(R * cos(a));

        lv_obj_t* label = g_chars[i];
        lv_label_set_text(label, chars[i]);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        // 注意：这里**不设字体** —— 见上面关于"字体指针会失效"的说明，让它继承屏幕的字体
        lv_obj_update_layout(label);

        lv_obj_align(label, LV_ALIGN_CENTER, x - cx, y - cy);
        // LVGL 的正角度是顺时针；取负 → 字顶朝向圆心（底部弧 = 笑脸形）
        const int rot = (int)lround(-(a * 180.0 / M_PI) * 10.0);
        lv_obj_set_style_transform_rotation(label, rot, 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);

        phi += half;
    }
    g_char_count = n;

    // 单侧展开是否超过上限：超了说明字太多，日志提醒一下（不强行缩，宁可让用户看到）
    const double span_deg = (total_rad / 2.0) * 180.0 / M_PI;
    if (span_deg > ATZ_ARC_TEXT_SPAN) {
        ESP_LOGW(TAG, "arc text spans %.0f deg each side (limit %d) -- %d chars, consider fewer",
                 span_deg, ATZ_ARC_TEXT_SPAN, n);
    }
}

void PollTimerCb(lv_timer_t* timer) {
    // 跑在 LVGL 任务里（锁由 lvgl_port 持有）
    (void)timer;
    if (g_layer == nullptr || !g_enabled) {
        return;
    }
    char text[160] = {0};
    if (atz_ui_copy_chat_text(text, sizeof(text)) == 0) {
        if (g_last_text[0] != '\0') {
            g_last_text[0] = '\0';
            LayoutText("");
        }
        return;
    }
    if (strcmp(text, g_last_text) == 0) {
        return;   // 没变就不重排
    }
    snprintf(g_last_text, sizeof(g_last_text), "%s", text);
    LayoutText(text);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════

void atz_arc_text_init(Display* display) {
    if (display == nullptr || g_layer != nullptr) {
        return;   // 幂等
    }
    g_display = display;

    DisplayLockGuard lock(display);
    lv_obj_t* screen = lv_screen_active();

    g_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(g_layer);
    lv_obj_set_size(g_layer, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(g_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(g_layer, LV_OBJ_FLAG_CLICKABLE);     // 别挡住点屏唤醒
    lv_obj_clear_flag(g_layer, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < ATZ_ARC_TEXT_MAX_CHARS; i++) {
        lv_obj_t* label = lv_label_create(g_layer);
        lv_label_set_text(label, "");
        lv_obj_set_size(label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        g_chars[i] = label;
    }

    g_timer = lv_timer_create(PollTimerCb, ATZ_ARC_TEXT_POLL_MS, nullptr);

    atz_arc_text_refresh();
    ESP_LOGI(TAG, "arc subtitle ready: R=%d px, +/-%d deg, max %d chars, %d ms poll",
             ATZ_ARC_TEXT_RADIUS, ATZ_ARC_TEXT_SPAN, ATZ_ARC_TEXT_MAX_CHARS, ATZ_ARC_TEXT_POLL_MS);
}

void atz_arc_text_on_theme_changed(void) {
    // 换主题/资源刷新会换字体对象：弧上的字必须重新量宽度、重新摆位
    atz_arc_text_refresh();
}

void atz_arc_text_set_enabled(bool on) {
    if (g_display == nullptr || g_layer == nullptr) {
        return;
    }
    DisplayLockGuard lock(g_display);
    g_enabled = on;
    atz_ui_set_stock_subtitle_visible(!on);   // 关掉弧形字幕时把上游直线字幕放回来
    if (on) {
        lv_obj_remove_flag(g_layer, LV_OBJ_FLAG_HIDDEN);
        g_last_text[0] = '\0';                // 强制下一帧重排
    } else {
        lv_obj_add_flag(g_layer, LV_OBJ_FLAG_HIDDEN);
    }
}

bool atz_arc_text_enabled(void) {
    return g_enabled;
}

void atz_arc_text_refresh(void) {
    if (g_display == nullptr || g_layer == nullptr) {
        return;
    }
    DisplayLockGuard lock(g_display);
    char text[160] = {0};
    atz_ui_copy_chat_text(text, sizeof(text));
    snprintf(g_last_text, sizeof(g_last_text), "%s", text);
    LayoutText(text);
}

#else   // ATZ_ARC_TEXT_ENABLE == 0

void atz_arc_text_init(Display* display) { (void)display; }
void atz_arc_text_set_enabled(bool on) { (void)on; }
bool atz_arc_text_enabled(void) { return false; }
void atz_arc_text_refresh(void) {}
void atz_arc_text_on_theme_changed(void) {}

#endif
