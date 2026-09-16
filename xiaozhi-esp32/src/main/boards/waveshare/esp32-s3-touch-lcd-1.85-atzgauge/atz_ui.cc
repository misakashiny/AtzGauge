// atz_ui.cc -- AtzGauge 主界面定制实现（主题包 + 车况条 + 语音控制）
//
// 设计约束（照 xiaozhi 的 AGENTS.md）：
//   * 不改上游 UI 代码：主题用上游的 LvglThemeManager 注册；车况条用「叠加式」覆写。
//   * 不在主循环里做重活：车况刷新交给 lv_timer（跑在 LVGL 任务上下文，锁由 lvgl_port 持有）。
//   * 跨任务修改 UI 必须加锁：MCP 工具回调不在 LVGL 任务里，所以 SetCarBarEnabled() 自己加锁。
//   * 幂等：SetupUI 可能被上游多次调用（历史上被重复调用过），构建函数必须能重入。

#include "atz_ui.h"
#include "atz_ui_config.h"

#include "assets/lang_config.h"
#include "atz_arc_text.h"
#include "atz_rpm_ring.h"
#include "atz_shot.h"
#include "espnow_link.h"
#include "lvgl_theme.h"
#include "mcp_server.h"
#include "obd_data_cache.h"
#include "settings.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#define TAG "AtzUi"

// 与上游 lcd_display.cc 同样的字体声明方式（BUILTIN_* 由 CMake 注入）
LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);
// ★ 真 30px 文本字体：组件的 CMakeLists 是 file(GLOB src/*.c)，所以这个字体的源码本来
//   就在编译范围内，只是没人引用而被链接器裁掉了。引用它就能拿到**清晰**的大号数字，
//   代价是固件体积增加（字体数据进 rodata）。
LV_FONT_DECLARE(font_noto_sans_basic_30_4);

// ═══════════════════════════════════════════════════════════════════════════
// 主题包
// ═══════════════════════════════════════════════════════════════════════════

namespace {

struct AtzPalette {
    uint32_t bg;
    uint32_t text;
    uint32_t chat_bg;
    uint32_t border;
    uint32_t low_battery;
    uint32_t user_bubble;
    uint32_t assist_bubble;
    uint32_t system_bubble;
    uint32_t system_text;
};

LvglTheme* BuildTheme(const char* name, const AtzPalette& p,
                      const std::shared_ptr<LvglFont>& text_font,
                      const std::shared_ptr<LvglFont>& icon_font,
                      const std::shared_ptr<LvglFont>& large_icon_font,
                      const std::shared_ptr<LvglFont>& emoji_font) {
    auto* theme = new LvglTheme(name);
    theme->set_background_color(lv_color_hex(p.bg));
    theme->set_text_color(lv_color_hex(p.text));
    theme->set_chat_background_color(lv_color_hex(p.chat_bg));
    theme->set_border_color(lv_color_hex(p.border));
    theme->set_low_battery_color(lv_color_hex(p.low_battery));
    theme->set_user_bubble_color(lv_color_hex(p.user_bubble));
    theme->set_assistant_bubble_color(lv_color_hex(p.assist_bubble));
    theme->set_system_bubble_color(lv_color_hex(p.system_bubble));
    theme->set_system_text_color(lv_color_hex(p.system_text));
    theme->set_text_font(text_font);
    theme->set_icon_font(icon_font);
    theme->set_large_icon_font(large_icon_font);
    theme->set_emoji_font(emoji_font);
    return theme;
}

}  // namespace

void atz_ui_register_themes(void) {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    // 字体对象与上游 InitializeLcdThemes() 用同一套（同一份字体可以被多个主题共享）
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    auto& manager = LvglThemeManager::GetInstance();

    const AtzPalette night{ATZ_UI_NIGHT_BG,        ATZ_UI_NIGHT_TEXT,
                           ATZ_UI_NIGHT_CHAT_BG,   ATZ_UI_NIGHT_BORDER,
                           ATZ_UI_NIGHT_LOW_BATTERY, ATZ_UI_NIGHT_USER_BUBBLE,
                           ATZ_UI_NIGHT_ASSIST_BUBBLE, ATZ_UI_NIGHT_SYS_BUBBLE,
                           ATZ_UI_NIGHT_SYS_TEXT};
    const AtzPalette day{ATZ_UI_DAY_BG,        ATZ_UI_DAY_TEXT,
                         ATZ_UI_DAY_CHAT_BG,   ATZ_UI_DAY_BORDER,
                         ATZ_UI_DAY_LOW_BATTERY, ATZ_UI_DAY_USER_BUBBLE,
                         ATZ_UI_DAY_ASSIST_BUBBLE, ATZ_UI_DAY_SYS_BUBBLE,
                         ATZ_UI_DAY_SYS_TEXT};
    const AtzPalette amber{ATZ_UI_AMBER_BG,        ATZ_UI_AMBER_TEXT,
                           ATZ_UI_AMBER_CHAT_BG,   ATZ_UI_AMBER_BORDER,
                           ATZ_UI_AMBER_LOW_BATTERY, ATZ_UI_AMBER_USER_BUBBLE,
                           ATZ_UI_AMBER_ASSIST_BUBBLE, ATZ_UI_AMBER_SYS_BUBBLE,
                           ATZ_UI_AMBER_SYS_TEXT};

    manager.RegisterTheme("atz-night", BuildTheme("atz-night", night, text_font, icon_font,
                                                  large_icon_font, emoji_font));
    manager.RegisterTheme("atz-day", BuildTheme("atz-day", day, text_font, icon_font,
                                                large_icon_font, emoji_font));
    manager.RegisterTheme("atz-amber", BuildTheme("atz-amber", amber, text_font, icon_font,
                                                  large_icon_font, emoji_font));

    // 主题默认值：做一次性迁移。
    //
    // 老设备（装本 UI 之前）NVS 里几乎一定存着上游默认的 "light" —— 那是白底，
    // 夜里很刺眼。所以第一次跑本代码时把主题改成 ATZ_UI_DEFAULT_THEME 并置位标记；
    // 之后用户自己切过的主题（语音切换会写同一个键）一律尊重，不再覆盖。
    Settings ui_settings(ATZ_UI_NVS_NAMESPACE, true);
    Settings display_settings("display", true);
    const int migrated = ui_settings.GetInt(ATZ_UI_NVS_KEY_MIGRATED, 0);
    std::string saved = display_settings.GetString("theme", "");
    if (migrated == 0 || saved.empty()) {
        display_settings.SetString("theme", ATZ_UI_DEFAULT_THEME);
        ui_settings.SetInt(ATZ_UI_NVS_KEY_MIGRATED, 1);
        ESP_LOGI(TAG, "first run of AtzGauge UI: theme -> %s (was '%s')", ATZ_UI_DEFAULT_THEME,
                 saved.c_str());
    } else {
        ESP_LOGI(TAG, "theme from NVS: %s (kept)", saved.c_str());
    }
    ESP_LOGI(TAG, "themes registered: atz-night / atz-day / atz-amber (+ upstream light/dark)");
}

// ═══════════════════════════════════════════════════════════════════════════
// 车况条文本
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// 无效值哨兵（与主表/obd_data_cache 约定一致，见 obd_data_cache.h 文件头）
constexpr int16_t kCoolantInvalid = -40;
constexpr int16_t kOilInvalid = -100;

void JoinSegments(char segs[][20], int count, char* dst, size_t dst_len) {
    dst[0] = '\0';
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            strncat(dst, "  ", dst_len - strlen(dst) - 1);
        }
        strncat(dst, segs[i], dst_len - strlen(dst) - 1);
    }
}

}  // namespace

int atz_ui_format_car_bar(char* buf, unsigned len) {
    if (buf == nullptr || len == 0) {
        return 0;
    }
    if (!espnow_slave_has_data()) {
        return snprintf(buf, len, "%s", ATZ_UI_NO_DATA_TEXT);
    }

    obd_data_snapshot_t s;
    obd_data_get_snapshot(&s);

    char segs[8][20];
    int n = 0;
#if ATZ_UI_CAR_BAR_SHOW_RPM
    snprintf(segs[n++], sizeof(segs[0]), "%u%s", (unsigned)s.rpm, ATZ_UI_UNIT_RPM);
#endif
#if ATZ_UI_CAR_BAR_SHOW_SPEED
    snprintf(segs[n++], sizeof(segs[0]), "%u%s", (unsigned)s.speed, ATZ_UI_UNIT_SPEED);
#endif
#if ATZ_UI_CAR_BAR_SHOW_COOLANT
    if (s.coolant_temp <= kCoolantInvalid) {
        snprintf(segs[n++], sizeof(segs[0]), "%s", ATZ_UI_NO_DATA_TEXT);
    } else {
        snprintf(segs[n++], sizeof(segs[0]), "%d%s", (int)s.coolant_temp, ATZ_UI_UNIT_TEMP);
    }
#endif
#if ATZ_UI_CAR_BAR_SHOW_BATTERY
    if (s.bat_mv <= 0) {
        snprintf(segs[n++], sizeof(segs[0]), "%s", ATZ_UI_NO_DATA_TEXT);
    } else {
        snprintf(segs[n++], sizeof(segs[0]), "%d.%d%s", (int)(s.bat_mv / 1000),
                 (int)((s.bat_mv % 1000) / 100), ATZ_UI_UNIT_VOLT);
    }
#endif
#if ATZ_UI_CAR_BAR_SHOW_OIL
    if (s.oil_temp <= kOilInvalid) {
        snprintf(segs[n++], sizeof(segs[0]), "%s", ATZ_UI_NO_DATA_TEXT);
    } else {
        snprintf(segs[n++], sizeof(segs[0]), "%d%s", (int)s.oil_temp, ATZ_UI_UNIT_TEMP);
    }
#endif
#if ATZ_UI_CAR_BAR_SHOW_LOAD
    if (s.load_pct < 0) {
        snprintf(segs[n++], sizeof(segs[0]), "%s", ATZ_UI_NO_DATA_TEXT);
    } else {
        snprintf(segs[n++], sizeof(segs[0]), "%d%s", (int)s.load_pct, ATZ_UI_UNIT_PERCENT);
    }
#endif
#if ATZ_UI_CAR_BAR_SHOW_TPS
    if (s.tps < 0) {
        snprintf(segs[n++], sizeof(segs[0]), "%s", ATZ_UI_NO_DATA_TEXT);
    } else {
        snprintf(segs[n++], sizeof(segs[0]), "%d%s", (int)s.tps, ATZ_UI_UNIT_PERCENT);
    }
#endif

    if (n == 0) {
        return snprintf(buf, len, "%s", ATZ_UI_NO_DATA_TEXT);
    }
    if (n <= 2) {
        char row[64];
        JoinSegments(segs, n, row, sizeof(row));
        return snprintf(buf, len, "%s", row);
    }

    // 3 个以上字段分两行，居中显示（圆屏上比一行长条更好读）
    const int first_row = (n + 1) / 2;
    char row1[64];
    char row2[64];
    JoinSegments(segs, first_row, row1, sizeof(row1));
    JoinSegments(segs + first_row, n - first_row, row2, sizeof(row2));
    return snprintf(buf, len, "%s\n%s", row1, row2);
}

// ═══════════════════════════════════════════════════════════════════════════
// AtzLcdDisplay
// ═══════════════════════════════════════════════════════════════════════════

void AtzLcdDisplay::SetupUI() {
    // 先让上游把原版布局建好（它内部自带显示锁），我们再做调整/叠加。
    SpiLcdDisplay::SetupUI();

    DisplayLockGuard lock(this);

    // 尺寸微调：始终生效（与 ATZ_UI_ENABLE 无关）
    ApplySizes();
    // 圆形屏布局修正：把上游整宽的顶栏/底栏收进圆的可视区
    ApplyRoundScreenLayout();
    LogSizes();

    // 屏幕外沿的转速圈（参考 obd_brz_gauge）：也始终生效，靠 ATZ_RPM_RING_ENABLE 单独控制
    atz_rpm_ring_init(this);
    ApplyRingTheme();

    // 逐字贴弧字幕：建好之后把上游那条直线字幕藏起来（见 ATZ_ARC_TEXT_HIDE_BAR）
    atz_arc_text_init(this);
#if ATZ_ARC_TEXT_ENABLE && ATZ_ARC_TEXT_HIDE_BAR
    SetStockSubtitleVisible(false);
#endif

#if ATZ_UI_ENABLE && ATZ_UI_CAR_BAR_ENABLE
    BuildCarBar();

    // 布局自检：把关键控件的真实几何打进日志。
    // 这个板子没有摄像头、PC 也看不到屏幕，所以"有没有重叠/越界"只能靠数字说话。
    lv_obj_update_layout(lv_screen_active());
    if (bottom_bar_ != nullptr) {
        lv_area_t a;
        lv_obj_get_coords(bottom_bar_, &a);
        ESP_LOGI(TAG, "layout: subtitle bar y=%d..%d (h=%d)", (int)a.y1, (int)a.y2,
                 (int)lv_obj_get_height(bottom_bar_));
    }
    if (car_bar_ != nullptr) {
        lv_area_t a;
        lv_obj_get_coords(car_bar_, &a);
        const int gap = (bottom_bar_ != nullptr) ? (int)(lv_obj_get_y(bottom_bar_) - a.y2) : -1;
        ESP_LOGI(TAG, "layout: car bar y=%d..%d (h=%d w=%d), gap above subtitle bar=%d px",
                 (int)a.y1, (int)a.y2, (int)lv_obj_get_height(car_bar_), (int)lv_obj_get_width(car_bar_),
                 gap);
    }
    atz_shot_log_ui_state(this);
#endif
}

// ═══════════════════════════════════════════════════════════════════════════
// 尺寸微调（表情图 / 顶部时间）
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// 尺寸微调的运行时值（默认取 atz_ui_config.h 的宏；可用 /size 端点改）
int g_emoji_scale_pct = ATZ_EMOJI_SCALE_PCT;
int g_clock_scale_pct = ATZ_CLOCK_SCALE_PCT;

// 由 atz_ui_bind() 记住的显示对象，供运行时改尺寸用
AtzLcdDisplay* g_bound_display = nullptr;

}  // namespace

void atz_ui_bind(AtzLcdDisplay* display) {
    g_bound_display = display;
}

// /size 端点用：把真实几何写出去（走 bind 记下的显示对象）
void atz_ui_describe_sizes(char* buf, size_t len) {
    if (buf == nullptr || len == 0) {
        return;
    }
    if (g_bound_display != nullptr) {
        g_bound_display->DescribeSizes(buf, len);
        return;
    }
    snprintf(buf, len, "(display not ready)\n");
}

// 逐字贴弧字幕要读上游那句字幕文本；chat_message_label_ 是上游的 protected 成员，
// 只有派生类能碰 → 在这里开一个小接口（带锁 + 拷贝，避免把内部指针漏出去）。
size_t atz_ui_copy_chat_text(char* buf, size_t len) {
    if (buf == nullptr || len == 0) {
        return 0;
    }
    buf[0] = '\0';
    if (g_bound_display == nullptr) {
        return 0;
    }
    return g_bound_display->CopyChatText(buf, len);
}

void atz_ui_set_stock_subtitle_visible(bool visible) {
    if (g_bound_display == nullptr) {
        return;
    }
    g_bound_display->SetStockSubtitleVisible(visible);
}

void atz_ui_get_scales(int* emoji_pct, int* clock_pct) {
    if (emoji_pct != nullptr) {
        *emoji_pct = g_emoji_scale_pct;
    }
    if (clock_pct != nullptr) {
        *clock_pct = g_clock_scale_pct;
    }
}

void atz_ui_set_scales(int emoji_pct, int clock_pct) {
    if (emoji_pct > 0) {
        g_emoji_scale_pct = emoji_pct;
    }
    if (clock_pct > 0) {
        g_clock_scale_pct = clock_pct;
    }
    if (g_bound_display != nullptr) {
        g_bound_display->ReapplySizes();   // 立即生效，不用重刷固件
    }
    ESP_LOGI(TAG, "scales: emoji=%d%% clock=%d%%", g_emoji_scale_pct, g_clock_scale_pct);
}

void AtzLcdDisplay::SetStatus(const char* status) {
    // 判断是不是时钟（HH:MM）—— 只有它用大号真字体；中文状态文字仍用主题字体（要 CJK）
    bool is_clock = false;
    if (status != nullptr && strlen(status) == 5 && status[2] == ':' &&
        isdigit((unsigned char)status[0]) && isdigit((unsigned char)status[1]) &&
        isdigit((unsigned char)status[3]) && isdigit((unsigned char)status[4])) {
        is_clock = true;
    }

    // ★★ 必须自己加显示锁！★★
    // 调用链是 Application::Run()(main 任务) → UpdateStatusBar() → SetStatus()，
    // 而 LVGL 的 lv_timer_handler() 跑在 lvgl_port 任务里。上游 SpiLcdDisplay::SetStatus()
    // 内部自带锁，但**我们这个 override 在它之前改控件** —— 那段没有锁保护。
    // 后果（踩过，现象很吓人）：main 任务与 lvgl 任务同时进 LVGL，撞上
    // lv_refr.c:286 那条 "Invalidate area is not allowed during rendering" 的断言路径，
    // main 任务卡死在 lv_inv_area 里 → IDLE0 饿死 → 每 10 秒一条 task_wdt，
    // 屏幕停在被卡住前的那一帧（一直是"登录服务器..."），时钟永远不出现。
    // lvgl_port 用的是递归互斥锁，所以这里锁住再调用上游（它内部还会再锁一次）是安全的。
    DisplayLockGuard lock(this);

    if (status_label_ != nullptr) {
        const lv_font_t* want = nullptr;
        if (is_clock && g_clock_scale_pct >= 125) {
            want = &font_noto_sans_basic_30_4;          // 真 30px，清晰（20px 的 150%）
        } else {
            auto* theme = static_cast<LvglTheme*>(current_theme_);
            if (theme != nullptr && theme->text_font() != nullptr) {
                want = theme->text_font()->font();
            }
        }
        if (want != nullptr) {
            lv_obj_set_style_text_font(status_label_, want, 0);
            // ★ 换字体必须同时把控件高度跟着字体的 line_height 改掉！★
            // 上游给 status_label_ 设的是 LV_LABEL_LONG_SCROLL_CIRCULAR：只要文字比控件框高，
            // LVGL 就会**一直上下滚动**（用户报的"时间在上下滚动"就是这个：时钟切到 30px
            // 字体后 line_height 43，而框还停在 20px 字体的 28 → 文字溢出 → 自动竖向跑马灯）。
            // 高度永远取当前字体的 line_height，就不会再滚。
            lv_obj_set_height(status_label_, want->line_height);
        }
        // 用真字体时不要再叠加 transform_zoom（那才会发虚）
        lv_obj_set_style_transform_zoom(status_label_, 256, 0);
    }

    SpiLcdDisplay::SetStatus(status);
}

// ═══════════════════════════════════════════════════════════════════════════
// 字幕文本 / 直线字幕开关（供逐字贴弧字幕使用）
// ═══════════════════════════════════════════════════════════════════════════

size_t AtzLcdDisplay::CopyChatText(char* buf, size_t len) {
    if (buf == nullptr || len == 0) {
        return 0;
    }
    DisplayLockGuard lock(this);
    buf[0] = '\0';
    if (chat_message_label_ == nullptr) {
        return 0;
    }
    const char* text = lv_label_get_text(chat_message_label_);
    if (text == nullptr) {
        return 0;
    }
    snprintf(buf, len, "%s", text);
    return strlen(buf);
}

void AtzLcdDisplay::SetStockSubtitleVisible(bool visible) {
    DisplayLockGuard lock(this);
    if (bottom_bar_ == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "stock subtitle bar %s", visible ? "shown" : "hidden");
}

// ═══════════════════════════════════════════════════════════════════════════
// 圆形屏布局修正
// ═══════════════════════════════════════════════════════════════════════════

void AtzLcdDisplay::ApplyRoundScreenLayout() {
#if ATZ_ROUND_ENABLE
    lv_obj_t* screen = lv_screen_active();

    // 顶栏：上游是整宽 360、flex 两端对齐 → 两端图标落在圆外
    // （实测 WiFi 图标在 x=8..27，而该高度可视区只有 x=125..235，等于完全看不见）。
    // 收窄成居中短条，两端图标就落回可视区内。
    if (top_bar_ != nullptr) {
        lv_obj_set_width(top_bar_, ATZ_ROUND_TOP_BAR_WIDTH);
        lv_obj_align_to(top_bar_, screen, LV_ALIGN_TOP_MID, 0, ATZ_ROUND_TOP_BAR_Y);
    }

    // 状态/时钟文字：下移到更宽的位置（y=44 处可视宽度约 235px，而 y=10 处只有 ~180px）
    // ATZ_CLOCK_Y_NUDGE：用户要求再往下挪 5px（转速环贴边后顶部视觉重心偏上）
    if (status_label_ != nullptr) {
        lv_obj_set_width(status_label_, ATZ_ROUND_STATUS_WIDTH);
        lv_obj_align_to(status_label_, screen, LV_ALIGN_TOP_MID, 0,
                        ATZ_ROUND_STATUS_Y + ATZ_CLOCK_Y_NUDGE);
    }
    if (notification_label_ != nullptr) {
        lv_obj_set_width(notification_label_, ATZ_ROUND_STATUS_WIDTH);
        lv_obj_align_to(notification_label_, screen, LV_ALIGN_TOP_MID, 0,
                        ATZ_ROUND_STATUS_Y + ATZ_CLOCK_Y_NUDGE);
    }

    // 字幕：上游整条贴底 → 底部最窄处只有约 180px 宽，两端必被切。
    // 收窄 + 整体上移，让文字落进可视区。
    if (bottom_bar_ != nullptr) {
        lv_obj_set_width(bottom_bar_, ATZ_ROUND_SUBTITLE_WIDTH +
                                        2 * lv_obj_get_style_pad_left(bottom_bar_, LV_PART_MAIN));
        lv_obj_align_to(bottom_bar_, screen, LV_ALIGN_BOTTOM_MID, 0, -ATZ_ROUND_SUBTITLE_Y_OFFSET);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_set_width(chat_message_label_, ATZ_ROUND_SUBTITLE_WIDTH);
    }

    // 顶栏图标：WiFi（network_label_）默认不显示 —— 圆屏顶栏本来就只有 180px，
    // 图标挤在时钟左边很占地方。图标本身不删，只是隐藏，随时能再打开
    // （语音「把 WiFi 图标打开」→ self.ui.set_status_icon，或改 atz_ui_config.h 的宏）。
    ApplyStatusIconVisibilities();

    lv_obj_update_layout(screen);
    ESP_LOGI(TAG, "round layout applied: top_bar w=%d, status y=%d w=%d, subtitle w=%d y_off=-%d",
             ATZ_ROUND_TOP_BAR_WIDTH, ATZ_ROUND_STATUS_Y, ATZ_ROUND_STATUS_WIDTH,
             ATZ_ROUND_SUBTITLE_WIDTH, ATZ_ROUND_SUBTITLE_Y_OFFSET);
#endif
}

void AtzLcdDisplay::ReapplySizes() {
    DisplayLockGuard lock(this);
    ApplySizes();
    LogSizes();
}

// ═══════════════════════════════════════════════════════════════════════════
// 顶栏图标开关（WiFi / 电量）
// ═══════════════════════════════════════════════════════════════════════════

// 默认值来自 atz_ui_config.h 的宏；用户用语音改过之后以 NVS 为准。
bool AtzLcdDisplay::StatusIconWanted(const char* which) const {
    Settings settings(ATZ_UI_NVS_NAMESPACE, true);
    if (strcmp(which, "wifi") == 0) {
        return settings.GetInt(ATZ_UI_NVS_KEY_ICON_WIFI, ATZ_SHOW_NETWORK_ICON) != 0;
    }
    if (strcmp(which, "battery") == 0) {
        return settings.GetInt(ATZ_UI_NVS_KEY_ICON_BATTERY, ATZ_SHOW_BATTERY_ICON) != 0;
    }
    return true;
}

// 调用者必须已持有显示锁（SetupUI / ApplyStatusIconVisibilities 都在锁内）
void AtzLcdDisplay::ApplyStatusIconVisibilities() {
    struct Target { const char* which; lv_obj_t* obj; };
    const Target targets[] = {
        {"wifi", network_label_},
        {"battery", battery_label_},
    };
    for (const auto& t : targets) {
        if (t.obj == nullptr) {
            continue;
        }
        if (StatusIconWanted(t.which)) {
            lv_obj_remove_flag(t.obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(t.obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    ESP_LOGI(TAG, "status icons: wifi=%d battery=%d (1=shown)", (int)StatusIconWanted("wifi"),
             (int)StatusIconWanted("battery"));
}

void AtzLcdDisplay::SetStatusIconVisible(const char* which, bool on) {
    if (which == nullptr) {
        return;
    }
    Settings settings(ATZ_UI_NVS_NAMESPACE, true);
    if (strcmp(which, "wifi") == 0) {
        settings.SetInt(ATZ_UI_NVS_KEY_ICON_WIFI, on ? 1 : 0);
    } else if (strcmp(which, "battery") == 0) {
        settings.SetInt(ATZ_UI_NVS_KEY_ICON_BATTERY, on ? 1 : 0);
    } else {
        return;
    }
    // 会被 MCP 工具回调调用（不在 LVGL 任务里）→ 必须自己加锁
    DisplayLockGuard lock(this);
    ApplyStatusIconVisibilities();
}

// ═══════════════════════════════════════════════════════════════════════════
// 转速圈换色（跟随主题）
// ═══════════════════════════════════════════════════════════════════════════

void AtzLcdDisplay::ApplyRingTheme() {
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    if (theme == nullptr) {
        return;
    }
    // 圈的主色 = 主题文字色：深色主题下是近白，浅色主题下是近黑 —— 两种底色都看得见
    atz_rpm_ring_apply_theme(lv_color_to_u32(theme->text_color()));
}

// /size 端点的回显：报**真实几何**，不报"素材 128px × 百分比"那种推算值。
// 背景：表情素材实际是 128×121（不是 128×128），可见的黄色圆又比素材框小一圈
// （PNG 四周有透明边距），所以推算出来的数字和外面对不上，会让人以为缩放没生效。
void AtzLcdDisplay::DescribeSizes(char* buf, size_t len) {
    if (buf == nullptr || len == 0) {
        return;
    }
    DisplayLockGuard lock(this);
    lv_obj_update_layout(lv_screen_active());

    int src_w = 0, src_h = 0, box_w = 0, box_h = 0;
    if (emoji_image_ != nullptr) {
        src_w = (int)lv_image_get_src_width(emoji_image_);
        src_h = (int)lv_image_get_src_height(emoji_image_);
        box_w = (int)lv_obj_get_width(emoji_image_);
        box_h = (int)lv_obj_get_height(emoji_image_);
    }
    int line_h = -1;
    int font_px = -1;
    if (status_label_ != nullptr) {
        const lv_font_t* f = lv_obj_get_style_text_font(status_label_, LV_PART_MAIN);
        if (f != nullptr) {
            line_h = (int)f->line_height;
            // 我们的两档字体：主题字 20px（line_height 28）/ 时钟专用 30px（line_height 43）
            font_px = (f == &font_noto_sans_basic_30_4) ? 30 : 20;
        }
    }
    snprintf(buf, len,
             "emoji=%d%% asset %dx%d -> box %dx%d px\n"
             "clock=%d%% font %dpx line_height %d (30px font is used only for HH:MM)\n",
             g_emoji_scale_pct, src_w, src_h, box_w, box_h, g_clock_scale_pct, font_px, line_h);
}

void AtzLcdDisplay::ApplySizes() {
    // ── 表情图 ────────────────────────────────────────────────────────────────
    // 两个要点（踩过的坑）：
    //   ① 不能只在 pct!=100 时设 —— 那样"改回 100%"会失效；
    //   ② **光设 scale 不够**：lv_image_set_scale() 是渲染期变换，控件自身的绘制框
    //      仍是原始 128px，放大后的图会被切掉。必须同时把控件尺寸也放大。
    if (emoji_image_ != nullptr) {
        lv_image_set_scale(emoji_image_, (uint32_t)(256 * g_emoji_scale_pct / 100));
        const int src_w = (int)lv_image_get_src_width(emoji_image_);
        const int src_h = (int)lv_image_get_src_height(emoji_image_);
        if (src_w > 0 && src_h > 0) {
            lv_obj_set_size(emoji_image_, src_w * g_emoji_scale_pct / 100,
                            src_h * g_emoji_scale_pct / 100);
        }
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        }
    }

    // ── 顶部时间/状态文字：就是 status_label_（时钟走 SetStatus()）──────────
    // ★ 不再用 transform_zoom 放大！那是**位图拉伸**，实测明显发虚。
    //   字号改由 SetStatus() 切到真 30px 字体（清晰），这里只把标签高度跟着字体走。
    if (status_label_ != nullptr) {
        lv_obj_set_style_transform_zoom(status_label_, 256, 0);
        const lv_font_t* font = lv_obj_get_style_text_font(status_label_, LV_PART_MAIN);
        if (font != nullptr) {
            lv_obj_set_height(status_label_, font->line_height);
        }
    }
    lv_obj_update_layout(lv_screen_active());
}

// 把缩放后的真实尺寸打进日志 —— 这样"放大了多少"是可核对的数字，而不是估计
void AtzLcdDisplay::LogSizes() {
    lv_obj_update_layout(lv_screen_active());
    if (emoji_image_ != nullptr) {
        ESP_LOGI(TAG, "size: emoji src %dx%d px | obj_scale=%d | transformed %dx%d px | pct=%d%%",
                 (int)lv_image_get_src_width(emoji_image_), (int)lv_image_get_src_height(emoji_image_),
                 (int)lv_image_get_scale(emoji_image_),
                 (int)lv_image_get_transformed_width(emoji_image_),
                 (int)lv_image_get_transformed_height(emoji_image_), g_emoji_scale_pct);
    }
    if (status_label_ != nullptr) {
        lv_area_t a;
        lv_obj_get_coords(status_label_, &a);
        const lv_font_t* font = lv_obj_get_style_text_font(status_label_, LV_PART_MAIN);
        ESP_LOGI(TAG, "size: clock label box %dx%d px | font line_height %d | pct=%d%%",
                 (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1),
                 font != nullptr ? (int)font->line_height : -1, g_clock_scale_pct);
    }
}

void AtzLcdDisplay::BuildCarBar() {
    if (car_bar_ != nullptr) {
        return;  // 幂等：重复调用直接返回（历史教训：绑定非幂等会把主任务刷到看门狗）
    }

    car_bar_enabled_ =
        Settings(ATZ_UI_NVS_NAMESPACE, false).GetInt(ATZ_UI_NVS_KEY_CAR_BAR, ATZ_UI_CAR_BAR_ENABLE) != 0;

    car_bar_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(car_bar_, ATZ_UI_CAR_BAR_WIDTH, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(car_bar_, ATZ_UI_CAR_BAR_RADIUS, 0);
    lv_obj_set_style_border_width(car_bar_, 1, 0);
    lv_obj_set_style_border_opa(car_bar_, (lv_opa_t)(ATZ_UI_CAR_BAR_BORDER_OPA * 255 / 100), 0);
    lv_obj_set_style_bg_opa(car_bar_, (lv_opa_t)(ATZ_UI_CAR_BAR_BG_OPA * 255 / 100), 0);
    lv_obj_set_style_pad_all(car_bar_, ATZ_UI_CAR_BAR_PAD, 0);
    lv_obj_set_scrollbar_mode(car_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(car_bar_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(car_bar_, LV_ALIGN_BOTTOM_MID, 0, -ATZ_UI_CAR_BAR_BOTTOM_GAP);

    car_label_ = lv_label_create(car_bar_);
    lv_label_set_text(car_label_, ATZ_UI_NO_DATA_TEXT);
    lv_obj_set_style_text_align(car_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(car_label_);

    ApplyCarBarStyle();
    UpdateCarBar();

    car_timer_ = lv_timer_create(CarBarTimerCb, ATZ_UI_CAR_BAR_UPDATE_MS, this);
    ESP_LOGI(TAG, "car bar built (enabled=%d, width=%d, gap=%d)", (int)car_bar_enabled_,
             ATZ_UI_CAR_BAR_WIDTH, ATZ_UI_CAR_BAR_BOTTOM_GAP);
}

void AtzLcdDisplay::ApplyCarBarStyle() {
    if (car_bar_ == nullptr || car_label_ == nullptr) {
        return;
    }
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    if (theme == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(car_bar_, theme->background_color(), 0);
    lv_obj_set_style_border_color(car_bar_, theme->border_color(), 0);
    lv_obj_set_style_text_color(car_label_, theme->text_color(), 0);
    if (theme->text_font() != nullptr && theme->text_font()->font() != nullptr) {
        lv_obj_set_style_text_font(car_label_, theme->text_font()->font(), 0);
    }
}

void AtzLcdDisplay::UpdateCarBar() {
    if (car_bar_ == nullptr || car_label_ == nullptr) {
        return;
    }
    if (!car_bar_enabled_) {
        lv_obj_add_flag(car_bar_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // 没有数据 / 数据过期 → 整条隐藏。宁可什么都不显示，也不显示过期值。
    const int64_t age_ms = espnow_slave_last_rx_age_ms();
    if (age_ms < 0 || age_ms > ATZ_UI_STALE_MS) {
        lv_obj_add_flag(car_bar_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char text[96];
    atz_ui_format_car_bar(text, sizeof(text));
    if (strcmp(text, car_text_) != 0) {
        // 只在内容真的变了才改文本：避免 500ms 一次的无效重绘（残影与功耗）
        strncpy(car_text_, text, sizeof(car_text_) - 1);
        car_text_[sizeof(car_text_) - 1] = '\0';
        lv_label_set_text(car_label_, car_text_);
    }
    lv_obj_remove_flag(car_bar_, LV_OBJ_FLAG_HIDDEN);
}

void AtzLcdDisplay::CarBarTimerCb(lv_timer_t* timer) {
    // lv_timer 回调运行在 LVGL 任务里，锁由 esp_lvgl_port 的任务循环持有，
    // 所以这里**不能**再取锁（会把显示锁套两层）。
    // 注：LVGL 9 的 lv_timer_t 是不透明类型，user_data 必须用 getter 取。
    auto* self = static_cast<AtzLcdDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->UpdateCarBar();
    }
}

void AtzLcdDisplay::SetCarBarEnabled(bool on) {
    car_bar_enabled_ = on;
    Settings(ATZ_UI_NVS_NAMESPACE, true).SetInt(ATZ_UI_NVS_KEY_CAR_BAR, on ? 1 : 0);
    // 本函数会被 MCP 工具回调调用（不在 LVGL 任务里）→ 必须自己加锁
    DisplayLockGuard lock(this);
    UpdateCarBar();
    ESP_LOGI(TAG, "car bar %s", on ? "on" : "off");
}

void AtzLcdDisplay::SetEmotion(const char* emotion) {
    // 上游启动时用的是 "robot_2"，而表情集里只有 21 个真实名字 → 会退回 40px 的字体图标。
    // 查不到就换成 ATZ_UI_EMOTION_FALLBACK（默认 neutral），让开机那一帧也是大表情。
    const char* mapped = emotion;
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    if (emotion != nullptr && theme != nullptr && theme->emoji_collection() != nullptr &&
        theme->emoji_collection()->GetEmojiImage(emotion) == nullptr) {
        if (theme->emoji_collection()->GetEmojiImage(ATZ_UI_EMOTION_FALLBACK) != nullptr) {
            mapped = ATZ_UI_EMOTION_FALLBACK;
        }
    }
    SpiLcdDisplay::SetEmotion(mapped);

    // 换表情图之后重新套用缩放（image 换了 src，缩放要再设一次才保险）
    DisplayLockGuard lock(this);
    ApplySizes();
}

void AtzLcdDisplay::SetEmojiCollection(std::shared_ptr<EmojiCollection> collection) {
    if (collection == nullptr) {
        return;
    }
    // 上游只在 light/dark 上挂了表情集；我们的主题在这里补上，否则表情永远走字体图标兜底。
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    if (theme != nullptr && theme->emoji_collection() == nullptr) {
        theme->set_emoji_collection(collection);
        ESP_LOGI(TAG, "emoji collection attached to theme '%s'", theme->name().c_str());
    }
}

void AtzLcdDisplay::SetTheme(Theme* theme) {
    SpiLcdDisplay::SetTheme(theme);  // 上游负责它自己那些控件（内部自带锁）

    // 兜底：如果换主题时资源系统已经把表情集挂到 light/dark 上了，就直接借过来。
    // （assets.cc 是"先 Apply 资源、再 display->SetTheme()"，所以这里通常能拿到。）
    auto* lvgl_theme = static_cast<LvglTheme*>(theme);
    if (lvgl_theme != nullptr && lvgl_theme->emoji_collection() == nullptr) {
        auto& manager = LvglThemeManager::GetInstance();
        for (const char* donor : {"light", "dark"}) {
            auto* src = manager.GetTheme(donor);
            if (src != nullptr && src->emoji_collection() != nullptr) {
                lvgl_theme->set_emoji_collection(src->emoji_collection());
                ESP_LOGI(TAG, "emoji collection inherited from '%s'", donor);
                break;
            }
        }
    }

    DisplayLockGuard lock(this);
    ApplyCarBarStyle();              // 我们这层要跟着换色
    ApplyRingTheme();                // 屏幕外沿的转速圈也要跟着换色
    atz_arc_text_on_theme_changed(); // 字体对象换了 → 弧形字幕要重新量宽重排
}

// ═══════════════════════════════════════════════════════════════════════════
// 台架预览：注入合成车况让车况条显示出来
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// 台架注入的"立即停车"标志：起新的模拟会清它，atz_ui_car_inject_stop() 会置它。
// 放在 namespace 最前面 —— CarBarDemoTask 也要用它。
volatile bool g_inject_stop = false;

void CarBarDemoTask(void* arg) {
    const int secs = (int)(intptr_t)arg;
    g_inject_stop = false;
    for (int i = 0; i < secs * 2; i++) {
        if (g_inject_stop) {
            ESP_LOGI(TAG, "car bar demo stopped by request");
            vTaskDelete(nullptr);
            return;
        }
        // 转速做一点小幅摆动，便于确认"刷新"确实在工作。
        // 注入走的是与真实收包同一条链路（espnow_slave_inject_test_packet）。
        espnow_slave_inject_test_packet((uint16_t)(3200 + (i % 5) * 40), 88, 92, -100, 38, 42, 27,
                                        14200);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "car bar demo finished");
    vTaskDelete(nullptr);
}

// ── 转速扫掠模拟 ───────────────────────────────────────────────────────────
// 一次循环 8 秒（80 个 100ms 刻度），模拟"怠速 → 拉转速 → 顶一下 → 松油门换挡 → 再拉 → 回怠速"：
//
//   刻度  0~ 9  怠速游车 850 → 1120
//        10~39  拉转速   1120 → 峰值
//        40~44  顶住     峰值
//        45~49  松油门   峰值 → 2400（换挡掉转速）
//        50~69  再拉     2400 → 峰值
//        70~79  回怠速   峰值 → 850
//
// 其它字段跟着动（油门/负荷跟转速，车速缓慢上下，水温油温慢慢爬），这样车况页看起来也是活的。
//
// ★ 停车（2026-09-16 用户报"环一直停不下来"）：
//   模拟一旦启动就会一直注入假数据，设备就"永久在演"。所以：
//     ① 上限从 300s 收到 ATZ_SIM_MAX_SECONDS（120s）；
//     ② 新增 atz_ui_car_inject_stop()，HTTP `/carbar?mode=stop` 与语音
//        `self.ui.car_bar_demo(mode="stop")` 都能立刻停；
//     ③ 启动新的模拟会先停掉上一个（同一个全局停止标志）。
void CarBarRevTask(void* arg) {
    const int packed = (int)(intptr_t)arg;
    const int secs = packed & 0xFFFF;
    const bool redline = (packed >> 16) != 0;
    const int top = redline ? 7000 : 6400;   // 6400 只到琥珀区；7000 会越过红线并触发音告警

    for (int tick = 0; tick < secs * 10; tick++) {
        if (g_inject_stop) {
            ESP_LOGI(TAG, "rev sim stopped by request");
            vTaskDelete(nullptr);
            return;
        }
        const int phase = tick % 80;
        const int prev_phase = (tick == 0) ? 0 : (tick - 1) % 80;
        int rpm = 850;
        if (phase < 10) {
            rpm = 850 + phase * 27;                                   // 怠速游车
        } else if (phase < 40) {
            rpm = 1120 + (phase - 10) * (top - 1120) / 30;             // 拉转速
        } else if (phase < 45) {
            rpm = top;                                                 // 顶住
        } else if (phase < 50) {
            rpm = top - (phase - 45) * (top - 2400) / 5;               // 松油门（换挡）
        } else if (phase < 70) {
            rpm = 2400 + (phase - 50) * (top - 2400) / 20;             // 再拉
        } else {
            rpm = top - (phase - 70) * (top - 850) / 10;               // 回怠速
        }
        if (rpm < 800) {
            rpm = 800;
        }

        // 转速越高 → 油门/负荷越大（0~100），车速按循环缓慢上下，温度慢慢往上爬
        const int load = 18 + (rpm - 800) * 62 / (top - 800);
        const int tps = 8 + (rpm - 800) * 74 / (top - 800);
        const int speed = 24 + (tick % 80) / 4;                        // 24 → 43 km/h 缓升
        const int coolant = 88 + (tick / 100);                         // 慢慢热起来
        const int oil = 96 + (tick / 60);
        const int intake = 28 + load / 6;
        const int bat_mv = 14200 - (rpm / 100) * 3;                    // 高转时电压略掉

        espnow_slave_inject_test_packet((uint16_t)rpm, (uint8_t)speed, (int16_t)coolant,
                                        (int16_t)oil, (int16_t)intake, (int16_t)load, (int16_t)tps,
                                        bat_mv);

        // 只在跨过"关键刻度"时打日志，避免 10Hz 刷屏
        if (phase != prev_phase + 1 && phase == 0) {
            ESP_LOGI(TAG, "rev sim: cycle restart");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "rev sim finished (%d s, peak %d rpm)", secs, top);
    vTaskDelete(nullptr);
}

}  // namespace

void atz_ui_car_bar_demo(int seconds) {
    if (seconds < 1) {
        seconds = 1;
    }
    if (seconds > ATZ_SIM_MAX_SECONDS) {
        seconds = ATZ_SIM_MAX_SECONDS;
    }
    g_inject_stop = false;      // 起新的之前先清标志（同时也就停掉了上一个）
    xTaskCreate(CarBarDemoTask, "atz_bar_demo", 3072, (void*)(intptr_t)seconds, 4, nullptr);
    ESP_LOGI(TAG, "car bar demo started for %d s (synthetic data)", seconds);
}

void atz_ui_car_rev_sim(int seconds, bool allow_redline) {
    if (seconds < 1) {
        seconds = 1;
    }
    if (seconds > ATZ_SIM_MAX_SECONDS) {
        seconds = ATZ_SIM_MAX_SECONDS;
    }
    g_inject_stop = false;      // 起新的之前先清标志（同时也就停掉了上一个）
    const int packed = (seconds & 0xFFFF) | (allow_redline ? (1 << 16) : 0);
    xTaskCreate(CarBarRevTask, "atz_rev_sim", 3584, (void*)(intptr_t)packed, 4, nullptr);
    ESP_LOGI(TAG, "rev sim started: %d s, peak %d rpm%s (max %d s; /carbar?mode=stop to stop)",
             seconds, allow_redline ? 7000 : 6400,
             allow_redline ? " (crosses the 6500 redline -> the audio alarm WILL fire)" : "",
             ATZ_SIM_MAX_SECONDS);
}

void atz_ui_car_inject_stop(void) {
    g_inject_stop = true;
    ESP_LOGI(TAG, "inject STOP requested (running sim will exit within 100 ms)");
}

// ═══════════════════════════════════════════════════════════════════════════
// 语音控制（MCP 工具）
// ═══════════════════════════════════════════════════════════════════════════

void atz_ui_register_tools(AtzLcdDisplay* display) {
    if (display == nullptr) {
        return;
    }
    auto& mcp_server = McpServer::GetInstance();

    // ═══ 下面这两个工具**与界面定制无关**，任何配置下都要注册 ═══════════════
    // 教训（2026-09-16）：这两个以前和 set_theme/set_car_bar 一起被 ATZ_UI_ENABLE
    // 关掉了，结果 ATZ_UI_ENABLE=0 时用户说"模拟一下转速"根本没工具可调 ——
    // 车况注入、转速环、顶栏图标在官方界面下也照样存在，工具就不该跟着一起消失。
    //
    // 台架预览：车况条/转速环只在"最近 2 秒收到过主表数据"时才动，而台架上通常没有主表。
    // 这个工具注入合成车况若干秒，让用户能把转速环、车况条、车况页调出来看。
    // 数值是假的，工具描述里已明确要求不得当成真实车况。
    mcp_server.AddTool(
        "self.ui.car_bar_demo",
        "Preview the on-screen vehicle data with SYNTHETIC values for N seconds. Use this ONLY when "
        "the user asks to see/check the screen layout, the RPM ring or the car data bar while the car "
        "is not connected. The readings are FAKE: never report them as the real vehicle state.\n"
        "mode=`demo` (default) shows steady fake values (RPM ~3200, 88 km/h, 92 C, 14.2 V). "
        "mode=`rev` plays a realistic 8-second rev cycle (idle -> rev up -> shift -> rev up -> idle, "
        "peak 6400 rpm) so the user can watch the RPM ring sweep and change colour. "
        "mode=`redline` is the same but peaks at 7000 rpm, which crosses the 6500 rpm limit and "
        "makes the device play its HIGH RPM alert out loud -- only use it if the user explicitly "
        "wants to test the alarm. "
        "mode=`stop` stops the simulation immediately (use it when the user says \"停\", \"别模拟了\", "
        "\"stop the simulation\"). The simulation also stops by itself after `seconds`.",
        PropertyList({Property("seconds", kPropertyTypeInteger, 5, 120),
                      Property("mode", kPropertyTypeString, std::string("demo"))}),
        [](const PropertyList& properties) -> ReturnValue {
            const int seconds = properties["seconds"].value<int>();
            std::string mode = properties["mode"].value<std::string>();
            for (auto& ch : mode) {
                ch = (char)tolower((unsigned char)ch);
            }
            if (mode == "rev" || mode == "sweep" || mode == "转速" || mode == "扫掠") {
                atz_ui_car_rev_sim(seconds, false);
            } else if (mode == "redline" || mode == "红线") {
                atz_ui_car_rev_sim(seconds, true);
            } else if (mode == "stop" || mode == "off" || mode == "停" || mode == "停止") {
                atz_ui_car_inject_stop();
            } else {
                atz_ui_car_bar_demo(seconds);
            }
            return true;
        });

    // ═══ 下面两个工具需要"定制界面"才成立：主题包只在 ATZ_UI_ENABLE=1 时注册，
    //     车况条控件也只在 ATZ_UI_ENABLE=1 时创建 → 一起用宏保护。 ═══════════════
#if ATZ_UI_ENABLE
    mcp_server.AddTool(
        "self.ui.set_theme",
        "Switch the screen theme (colors) of this device. Call this whenever the user talks about "
        "the screen being too bright/dark, or asks for a different look.\n"
        "Available themes:\n"
        "  `atz-night` - dark cockpit theme, recommended at night (device default)\n"
        "  `atz-day`   - bright high-contrast theme, best in daylight\n"
        "  `atz-amber` - amber on black, the easiest on the eyes at night\n"
        "  `light` / `dark` - the upstream stock themes\n"
        "Mapping: \"太亮/夜里/夜间/晚上\" -> `atz-night` or `atz-amber`; \"白天/看不清/太暗\" -> "
        "`atz-day`. The choice is stored on the device and survives a reboot.",
        PropertyList({Property("theme", kPropertyTypeString)}),
        [display](const PropertyList& properties) -> ReturnValue {
            auto name = properties["theme"].value<std::string>();
            auto* theme = LvglThemeManager::GetInstance().GetTheme(name);
            if (theme == nullptr) {
                ESP_LOGW(TAG, "unknown theme: %s", name.c_str());
                return false;
            }
            display->SetTheme(theme);
            ESP_LOGI(TAG, "theme switched to %s", name.c_str());
            return true;
        });

    mcp_server.AddTool(
        "self.ui.set_car_bar",
        "Turn the live vehicle-data bar on the main screen on or off. It shows engine RPM, speed, "
        "coolant temperature and battery voltage received from the car instrument over ESP-NOW. "
        "Use `false` when the user wants a cleaner screen, `true` to bring it back. The setting is "
        "stored on the device and survives a reboot.",
        PropertyList({Property("enabled", kPropertyTypeBoolean, true)}),
        [display](const PropertyList& properties) -> ReturnValue {
            bool enabled = properties["enabled"].value<bool>();
            display->SetCarBarEnabled(enabled);
            return true;
        });
#endif  // ATZ_UI_ENABLE

    // 转速环开关：用户说"把转速环关掉/打开"就调它（写 NVS，重启仍生效）
    mcp_server.AddTool(
        "self.ui.set_rpm_ring",
        "Show or hide the RPM ring drawn along the edge of the screen (the ring that fills up "
        "with engine RPM). Call this when the user says \"把转速环关掉\", \"不要那个圈\", "
        "\"关掉转速环\", \"hide the rpm ring\", \"把转速环打开\", \"show the rpm ring\". "
        "The choice is stored on the device and survives a reboot.",
        PropertyList({Property("enabled", kPropertyTypeBoolean, true)}),
        [](const PropertyList& properties) -> ReturnValue {
            const bool on = properties["enabled"].value<bool>();
            atz_rpm_ring_set_enabled(on);
            atz_rpm_ring_save_enabled(on);      // 写 NVS：重启后仍生效
            return std::string(on ? "rpm ring is now ON" : "rpm ring is now OFF");
        });

    // 顶栏图标（WiFi / 电量）：圆屏顶栏很窄，默认不显示 WiFi 图标。
    // 单独做成工具，是为了用户哪天想看网络状态时不用重新刷固件。
    mcp_server.AddTool(
        "self.ui.set_status_icon",
        "Show or hide a small icon in the status bar at the top of the screen. "
        "`icon` accepts: \"wifi\" (network icon) or \"battery\". "
        "Call this when the user says things like \"把 WiFi 图标关掉/打开\", \"不要显示电量\", "
        "\"hide the wifi icon\", \"show battery\". The choice is stored on the device and survives "
        "a reboot. Note: the wifi icon is HIDDEN by default on this round-screen device.",
        PropertyList({Property("icon", kPropertyTypeString), Property("visible", kPropertyTypeBoolean, true)}),
        [display](const PropertyList& properties) -> ReturnValue {
            auto name = properties["icon"].value<std::string>();
            bool visible = properties["visible"].value<bool>();
            // 容忍中文/别名，避免大模型把 "wifi"/"Wi-Fi"/"网络" 传成不同写法
            std::string key = name;
            for (auto& ch : key) {
                ch = (char)tolower((unsigned char)ch);
            }
            if (key == "wifi" || key == "wi-fi" || key == "network" || key == "网络" ||
                key == "无线") {
                key = "wifi";
            } else if (key == "battery" || key == "bat" || key == "电量" || key == "电池") {
                key = "battery";
            } else {
                ESP_LOGW(TAG, "unknown status icon: %s", name.c_str());
                return false;
            }
            display->SetStatusIconVisible(key.c_str(), visible);
            return true;
        });
}
