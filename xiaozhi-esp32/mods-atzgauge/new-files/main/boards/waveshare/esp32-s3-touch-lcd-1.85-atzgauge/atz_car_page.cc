// atz_car_page.cc -- 「车况」整屏页面实现
//
// 布局（360x360 圆屏）：
//
//        ● 车况 10Hz                      y=-150   链路状态（绝对定位）
//      ┌────────────────────────────────┐
//      │   转速          车速            │   hero 行：只放两个主数值
//      │  3210 rpm      87 km/h         │   数值用真 30px 字体（清晰，不拉伸位图）
//      ├────────────────────────────────┤
//      │ 水温 91C   │ 油温 104C          │   grid：其余字段 2 列自动换行
//      │ 进气 38C   │ 负荷 42%           │   每格 = 「名字 + 数值」一行
//      │ 节气门 27% │ 电压 14.2V         │
//      │ 油压 4.2bar│ 空燃比 14.7        │
//      └────────────────────────────────┘
//
// ★ 布局用 **flex**（流式）而不是写死 y 坐标，这样才能做到"语音关掉某个项目之后
//   界面自动重排、不留空位"（需求：UI 要跟随显示的情况去自动匹配）。
//   LVGL 的 flex 会自动跳过 hidden 的子控件，所以隐藏 = 直接加 HIDDEN 标志即可。
//
// 颜色沿用 obd_brz_gauge 的深色仪表风格（黑底 + 白值 + 青/绿高亮 + 超限变色）。

#include "atz_car_page.h"

#include "assets/lang_config.h"
#include "atz_rpm_ring.h"
#include "atz_ui_config.h"
#include "display.h"
#include "espnow_link.h"
#include "lvgl_theme.h"
#include "mcp_server.h"
#include "obd_data_cache.h"
#include "settings.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#define TAG "AtzCarPage"

LV_FONT_DECLARE(font_noto_sans_basic_30_4);   // 主数值用（清晰的真 30px 字体）

// ── 可调参数 ────────────────────────────────────────────────────────────────
#define ATZ_CAR_PAGE_REFRESH_MS   300     // 刷新周期
#define ATZ_CAR_PAGE_STALE_MS     2000    // 超过这么久没数据 → 整页视为"无数据"
#define ATZ_CAR_PAGE_COL_W        300     // 内容列宽（圆屏中部最宽约 350）
#define ATZ_CAR_PAGE_CELL_W       142     // 网格单元宽（两列 + 间距正好 ≤ 列宽）
#define ATZ_CAR_PAGE_GRID_GAP     8       // 网格列间距
#define ATZ_CAR_PAGE_GRID_ROW_GAP 4       // 网格行间距

// 配色（参考 obd_brz_gauge 的深色仪表风）
#define ATZ_PAGE_BG        0x000000
#define ATZ_PAGE_LABEL     0x8A939E   // 项目名（灰）
#define ATZ_PAGE_VALUE     0xFFFFFF   // 数值（白）
#define ATZ_PAGE_RPM       0x00E5FF   // 转速（青）
#define ATZ_PAGE_SPEED     0x00CC66   // 车速（绿，与仪表项目同色）
#define ATZ_PAGE_WARN      0xFFA000   // 接近上限（琥珀）
#define ATZ_PAGE_ALARM     0xFF3B30   // 超限（红）
#define ATZ_PAGE_STALE     0x5A5A5A   // 无数据（暗灰）
#define ATZ_PAGE_OK        0x00CC66   // 链路正常（绿点）
#define ATZ_PAGE_BAD       0xFF3B30   // 链路无数据（红点）

namespace {

Display* g_display = nullptr;
lv_obj_t* g_page = nullptr;
lv_obj_t* g_col = nullptr;        // 内容列（flex column，居中）
lv_obj_t* g_hero_row = nullptr;   // 主数值行（flex row）
lv_obj_t* g_grid = nullptr;       // 其余字段（flex row wrap）
lv_timer_t* g_timer = nullptr;
lv_obj_t* g_link_label = nullptr;
lv_obj_t* g_stale_label = nullptr;

struct FieldUi {
    lv_obj_t* cell = nullptr;        // 整格容器（隐藏它就等于"这一项不显示"）
    lv_obj_t* value = nullptr;       // 数值标签
    lv_obj_t* unit = nullptr;        // 单位标签（只有主数值有；其余并进 value 文本）
    uint32_t base_color = ATZ_PAGE_VALUE;
    bool visible = true;
};
FieldUi g_fields[10];

// 10 个字段，顺序与屏幕上的排布一致
enum FieldIndex {
    kRpm = 0, kSpeed, kCoolant, kOil, kIntake, kLoad, kTps, kVolt, kOilPress, kAfr, kFieldCount
};

// 语音/HTTP 用的字段名（英文 token ↔ 中文别名 ↔ 字段）
struct FieldName {
    const char* token;      // 存进 NVS 的规范名
    const char* alias[5];   // 可接受的写法（大模型可能传中文，也可能传别的不规范英文）
};
const FieldName kFieldNames[kFieldCount] = {
    {"rpm",      {"rpm", "转速", "engine speed", nullptr, nullptr}},
    {"speed",    {"speed", "车速", "时速", "vehicle speed", nullptr}},
    {"coolant",  {"coolant", "水温", "冷却液", "water temp", nullptr}},
    {"oil",      {"oil", "油温", "机油温度", "oil temp", nullptr}},
    {"intake",   {"intake", "进气", "进气温度", "intake temp", nullptr}},
    {"load",     {"load", "负荷", "发动机负荷", "engine load", nullptr}},
    {"tps",      {"tps", "节气门", "油门", "throttle", nullptr}},
    {"volt",     {"volt", "voltage", "电压", "电瓶", "battery"}},
    {"oilpress", {"oilpress", "oil press", "油压", "机油压力", nullptr}},
    {"afr",      {"afr", "空燃比", "混合比", nullptr, nullptr}},
};

struct FieldValue {
    char text[24];
    int severity;   // -1 = 无数据(哨兵值)，0 = 正常，1 = 接近上限，2 = 超限
};

// 无效值哨兵与 obd_data_cache.h 的约定一致
constexpr int16_t kCoolantInvalid = -40;
constexpr int16_t kOilInvalid = -100;
constexpr int16_t kIntakeInvalid = -40;

FieldValue ReadField(FieldIndex idx, const obd_data_snapshot_t& s) {
    FieldValue v{};
    v.severity = 0;
    switch (idx) {
        case kRpm:
            snprintf(v.text, sizeof(v.text), "%u", (unsigned)s.rpm);
            if (s.rpm >= 6500) v.severity = 2;        // 与 car_alarm 的阈值一致
            else if (s.rpm >= 5500) v.severity = 1;
            break;
        case kSpeed:
            snprintf(v.text, sizeof(v.text), "%u", (unsigned)s.speed);
            break;
        case kCoolant:
            if (s.coolant_temp <= kCoolantInvalid) { snprintf(v.text, sizeof(v.text), "--"); v.severity = -1; }
            else {
                snprintf(v.text, sizeof(v.text), "%d", (int)s.coolant_temp);
                if (s.coolant_temp >= 108) v.severity = 2;
                else if (s.coolant_temp >= 100) v.severity = 1;
            }
            break;
        case kOil:
            if (s.oil_temp <= kOilInvalid) { snprintf(v.text, sizeof(v.text), "--"); v.severity = -1; }
            else {
                snprintf(v.text, sizeof(v.text), "%d", (int)s.oil_temp);
                if (s.oil_temp >= 125) v.severity = 2;
                else if (s.oil_temp >= 115) v.severity = 1;
            }
            break;
        case kIntake:
            if (s.intake_temp <= kIntakeInvalid) { snprintf(v.text, sizeof(v.text), "--"); v.severity = -1; }
            else snprintf(v.text, sizeof(v.text), "%d", (int)s.intake_temp);
            break;
        case kLoad:
            if (s.load_pct < 0) { snprintf(v.text, sizeof(v.text), "--"); v.severity = -1; }
            else snprintf(v.text, sizeof(v.text), "%d", (int)s.load_pct);
            break;
        case kTps:
            if (s.tps < 0) { snprintf(v.text, sizeof(v.text), "--"); v.severity = -1; }
            else snprintf(v.text, sizeof(v.text), "%d", (int)s.tps);
            break;
        case kVolt:
            if (s.bat_mv <= 0) { snprintf(v.text, sizeof(v.text), "--"); v.severity = -1; }
            else {
                snprintf(v.text, sizeof(v.text), "%d.%d", (int)(s.bat_mv / 1000),
                         (int)((s.bat_mv % 1000) / 100));
                if (s.bat_mv <= 11500 || s.bat_mv >= 15000) v.severity = 2;   // 与 car_alarm 一致
                else if (s.bat_mv <= 12000 || s.bat_mv >= 14700) v.severity = 1;
            }
            break;
        case kOilPress:
            if (s.oil_pressure_x10 < 0) { snprintf(v.text, sizeof(v.text), "--"); v.severity = -1; }
            else snprintf(v.text, sizeof(v.text), "%d.%d", (int)(s.oil_pressure_x10 / 10),
                          (int)(s.oil_pressure_x10 % 10));
            break;
        case kAfr:
            if (s.afr_x100 < 0) { snprintf(v.text, sizeof(v.text), "--"); v.severity = -1; }
            else snprintf(v.text, sizeof(v.text), "%d.%02d", (int)(s.afr_x100 / 100),
                          (int)(s.afr_x100 % 100));
            break;
        default:
            snprintf(v.text, sizeof(v.text), "--");
            v.severity = -1;
            break;
    }
    return v;
}

const char* UnitOf(FieldIndex idx) {
    switch (idx) {
        case kRpm: return "rpm";
        case kSpeed: return "km/h";
        case kCoolant:
        case kOil:
        case kIntake: return "C";
        case kLoad:
        case kTps: return "%";
        case kVolt: return "V";
        case kOilPress: return "bar";
        case kAfr: return "";
        default: return "";
    }
}

const char* NameOf(FieldIndex idx) {
    switch (idx) {
        case kRpm: return "转速";
        case kSpeed: return "车速";
        case kCoolant: return "水温";
        case kOil: return "油温";
        case kIntake: return "进气";
        case kLoad: return "负荷";
        case kTps: return "节气门";
        case kVolt: return "电压";
        case kOilPress: return "油压";
        case kAfr: return "空燃比";
        default: return "?";
    }
}

bool IsHero(FieldIndex idx) {
    return idx == kRpm || idx == kSpeed;
}

// ── 字段名解析：把语音/HTTP 传来的写法归一到规范 token ──────────────────────
// 返回 -1 表示不认识。
int LookupField(const std::string& raw) {
    std::string key;
    key.reserve(raw.size());
    for (char ch : raw) {
        if (ch == ' ' || ch == '-' || ch == '_' || ch == '\t') {
            continue;   // "oil temp" / "oil_temp" / "oil-temp" 都当成 oiltemp
        }
        key.push_back((char)tolower((unsigned char)ch));
    }
    if (key.empty()) {
        return -1;
    }
    // 去掉 "temp"/"温度" 之类后缀再比一次，容忍 "intake temp" → "intake"
    static const char* kSuffixes[] = {"temperature", "temp", "温度", "传感器"};
    for (int i = 0; i < kFieldCount; i++) {
        for (const char* alias : kFieldNames[i].alias) {
            if (alias == nullptr) {
                continue;
            }
            std::string a;
            for (const char* p = alias; *p != '\0'; p++) {
                if (*p == ' ' || *p == '-' || *p == '_') {
                    continue;
                }
                a.push_back((char)tolower((unsigned char)*p));
            }
            if (key == a) {
                return i;
            }
        }
    }
    for (int i = 0; i < kFieldCount; i++) {
        for (const char* alias : kFieldNames[i].alias) {
            if (alias == nullptr) {
                continue;
            }
            std::string a;
            for (const char* p = alias; *p != '\0'; p++) {
                if (*p == ' ' || *p == '-' || *p == '_') {
                    continue;
                }
                a.push_back((char)tolower((unsigned char)*p));
            }
            for (const char* suffix : kSuffixes) {
                if (a.size() > strlen(suffix) &&
                    a.compare(a.size() - strlen(suffix), strlen(suffix), suffix) == 0) {
                    a = a.substr(0, a.size() - strlen(suffix));
                    if (key == a) {
                        return i;
                    }
                }
            }
        }
    }
    return -1;
}

std::string JoinVisible(void) {
    std::string out;
    for (int i = 0; i < kFieldCount; i++) {
        if (!g_fields[i].visible) {
            continue;
        }
        if (!out.empty()) {
            out += ",";
        }
        out += kFieldNames[i].token;
    }
    return out;
}

void SaveVisible(void) {
    Settings settings(ATZ_UI_NVS_NAMESPACE, true);
    settings.SetString(ATZ_UI_NVS_KEY_PAGE_FIELDS, JoinVisible());
}

// 读 NVS；没存过就"全部显示"（与加入本功能之前的行为一致）
void LoadVisible(void) {
    for (int i = 0; i < kFieldCount; i++) {
        g_fields[i].visible = true;
    }
    Settings settings(ATZ_UI_NVS_NAMESPACE, true);
    std::string saved = settings.GetString(ATZ_UI_NVS_KEY_PAGE_FIELDS, "");
    if (saved.empty()) {
        return;
    }
    if (saved == "all") {
        return;
    }
    bool any[kFieldCount] = {};
    // 逐个 token 匹配
    size_t pos = 0;
    while (pos <= saved.size()) {
        size_t comma = saved.find(',', pos);
        std::string tok = saved.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        int idx = LookupField(tok);
        if (idx >= 0) {
            any[idx] = true;
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    for (int i = 0; i < kFieldCount; i++) {
        g_fields[i].visible = any[i];
    }
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, uint32_t color, const lv_font_t* font) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (font != nullptr) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    return label;
}

// 屏幕中心坐标 → 控件（用 CENTER 对齐，保证圆屏边缘不被切）
void PlaceAt(lv_obj_t* obj, int dx, int dy) {
    lv_obj_align(obj, LV_ALIGN_CENTER, dx, dy);
}

// 透明容器：flex 用它来排版，不要默认主题的背景/描边/内边距
lv_obj_t* MakeBox(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    return box;
}

void BuildCell(FieldIndex idx) {
    const bool hero = IsHero(idx);
    lv_obj_t* parent = hero ? g_hero_row : g_grid;

    lv_obj_t* cell = MakeBox(parent);
    lv_obj_set_width(cell, ATZ_CAR_PAGE_CELL_W);
    lv_obj_set_height(cell, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cell, hero ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cell, 4, 0);

    // 项目名
    lv_obj_t* name = MakeLabel(cell, NameOf(idx), ATZ_PAGE_LABEL, nullptr);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);

    // 数值
    lv_obj_t* value = MakeLabel(cell, "--",
                                hero ? (idx == kRpm ? ATZ_PAGE_RPM : ATZ_PAGE_SPEED) : ATZ_PAGE_VALUE,
                                hero ? &font_noto_sans_basic_30_4 : nullptr);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);

    g_fields[idx].cell = cell;
    g_fields[idx].value = value;
    g_fields[idx].base_color = (idx == kRpm) ? ATZ_PAGE_RPM : (idx == kSpeed ? ATZ_PAGE_SPEED : ATZ_PAGE_VALUE);

    // 主数值的单位单独一行小字；其余字段的单位直接拼在数值后面（省高度）
    if (hero) {
        g_fields[idx].unit = MakeLabel(cell, UnitOf(idx), ATZ_PAGE_LABEL, nullptr);
    } else {
        g_fields[idx].unit = nullptr;
    }
}

void BuildPage(lv_obj_t* screen) {
    g_page = lv_obj_create(screen);
    lv_obj_set_size(g_page, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(g_page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(g_page, 0, 0);
    lv_obj_set_style_border_width(g_page, 0, 0);
    lv_obj_set_style_pad_all(g_page, 0, 0);
    lv_obj_set_style_bg_color(g_page, lv_color_hex(ATZ_PAGE_BG), 0);
    lv_obj_set_style_bg_opa(g_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(g_page, LV_SCROLLBAR_MODE_OFF);

    // ── 顶部：链路状态 ─────────────────────────────────────────────────────
    // ★ 圆屏约束：y=-150 处的可用宽度只有 2*sqrt(180²-150²) ≈ 198px（20px 字约 10 个汉字），
    //   所以这里的文字必须短。
    g_link_label = MakeLabel(g_page, "● 车况", ATZ_PAGE_BAD, nullptr);
    lv_obj_set_style_text_align(g_link_label, LV_TEXT_ALIGN_CENTER, 0);
    PlaceAt(g_link_label, 0, -150);

    // ── 内容列（flex column，整体居中；隐藏字段后自动收缩并保持居中） ──────
    g_col = MakeBox(g_page);
    lv_obj_set_width(g_col, ATZ_CAR_PAGE_COL_W);
    lv_obj_set_height(g_col, LV_SIZE_CONTENT);
    lv_obj_align(g_col, LV_ALIGN_CENTER, 0, 6);
    lv_obj_set_flex_flow(g_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_col, 8, 0);

    g_hero_row = MakeBox(g_col);
    lv_obj_set_width(g_hero_row, ATZ_CAR_PAGE_COL_W);
    lv_obj_set_height(g_hero_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_hero_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_hero_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_hero_row, ATZ_CAR_PAGE_GRID_GAP, 0);

    g_grid = MakeBox(g_col);
    lv_obj_set_width(g_grid, ATZ_CAR_PAGE_COL_W);
    lv_obj_set_height(g_grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(g_grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_grid, ATZ_CAR_PAGE_GRID_GAP, 0);
    lv_obj_set_style_pad_row(g_grid, ATZ_CAR_PAGE_GRID_ROW_GAP, 0);

    // 先建主数值，再建网格（顺序只影响 flex 的排布，不影响可见性）
    for (int i = 0; i < kFieldCount; i++) {
        BuildCell((FieldIndex)i);
    }

    // ── 底部：无数据提示（默认隐藏；y=+152 处可用宽度约 192px，文字要短） ──
    g_stale_label = MakeLabel(g_page, "检查主表与信道", ATZ_PAGE_ALARM, nullptr);
    lv_obj_set_style_text_align(g_stale_label, LV_TEXT_ALIGN_CENTER, 0);
    PlaceAt(g_stale_label, 0, 152);
    lv_obj_add_flag(g_stale_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(g_page, LV_OBJ_FLAG_HIDDEN);   // 默认隐藏
    ESP_LOGI(TAG, "car page built");
}

// 把"哪些字段可见"落到控件上。LVGL 的 flex 会自动跳过 hidden 的子控件，
// 所以这里只要打/清 HIDDEN 标志，剩下的重排是自动的（这正是需求要的"自动匹配"）。
void ApplyFieldVisibility(void) {
    for (int i = 0; i < kFieldCount; i++) {
        if (g_fields[i].cell == nullptr) {
            continue;
        }
        if (g_fields[i].visible) {
            lv_obj_remove_flag(g_fields[i].cell, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_fields[i].cell, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (g_col != nullptr) {
        lv_obj_update_layout(g_col);
    }
}

void RefreshPage(void) {
    if (g_page == nullptr) {
        return;
    }
    const bool fresh = espnow_slave_has_data() &&
                       espnow_slave_last_rx_age_ms() >= 0 &&
                       espnow_slave_last_rx_age_ms() <= ATZ_CAR_PAGE_STALE_MS;

    // 顶部链路状态（★ 圆屏：文字要短，y=-150 处可用宽度约 198px）
    if (g_link_label != nullptr) {
        if (fresh) {
            lv_label_set_text(g_link_label, "● 车况 10Hz");
            lv_obj_set_style_text_color(g_link_label, lv_color_hex(ATZ_PAGE_OK), 0);
        } else if (espnow_slave_last_rx_age_ms() < 0) {
            lv_label_set_text(g_link_label, "● 车况 未连接");
            lv_obj_set_style_text_color(g_link_label, lv_color_hex(ATZ_PAGE_BAD), 0);
        } else {
            lv_label_set_text(g_link_label, "● 车况 无数据");
            lv_obj_set_style_text_color(g_link_label, lv_color_hex(ATZ_PAGE_BAD), 0);
        }
    }

    obd_data_snapshot_t s = {};
    obd_data_get_snapshot(&s);

    for (int i = 0; i < kFieldCount; i++) {
        if (g_fields[i].value == nullptr || !g_fields[i].visible) {
            continue;
        }
        const FieldValue v = ReadField((FieldIndex)i, s);
        char text[32];
        if (!fresh) {
            // 数据不新鲜：不显示具体数值，避免把过期值当实时值
            snprintf(text, sizeof(text), "--");
            lv_obj_set_style_text_color(g_fields[i].value, lv_color_hex(ATZ_PAGE_STALE), 0);
        } else {
            const char* unit = UnitOf((FieldIndex)i);
            // 主数值（转速/车速）的单位在下一行单独显示，这里就不要再拼一次
            if (unit[0] != '\0' && !IsHero((FieldIndex)i)) {
                snprintf(text, sizeof(text), "%s %s", v.text, unit);
            } else {
                snprintf(text, sizeof(text), "%s", v.text);
            }
            uint32_t color = g_fields[i].base_color;
            if (v.severity == 2) color = ATZ_PAGE_ALARM;
            else if (v.severity == 1) color = ATZ_PAGE_WARN;
            else if (v.severity < 0) color = ATZ_PAGE_LABEL;
            lv_obj_set_style_text_color(g_fields[i].value, lv_color_hex(color), 0);
        }
        if (strcmp(lv_label_get_text(g_fields[i].value), text) != 0) {
            lv_label_set_text(g_fields[i].value, text);   // 只在变化时改，减少重绘
        }
    }

    if (g_stale_label != nullptr) {
        if (fresh) {
            lv_obj_add_flag(g_stale_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(g_stale_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void PageTimerCb(lv_timer_t* timer) {
    // 跑在 LVGL 任务里，锁由 lvgl_port 持有，这里不能再取锁
    (void)timer;
    if (g_page != nullptr && !lv_obj_has_flag(g_page, LV_OBJ_FLAG_HIDDEN)) {
        RefreshPage();
    }
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 语音可调的字段开关
// ═══════════════════════════════════════════════════════════════════════════

std::string atz_car_page_set_field(const std::string& field, bool visible) {
    const int idx = LookupField(field);
    if (idx < 0) {
        return std::string("unknown field: ") + field;
    }
    if (g_display != nullptr) {
        DisplayLockGuard lock(g_display);
        g_fields[idx].visible = visible;
        ApplyFieldVisibility();
        RefreshPage();
    } else {
        g_fields[idx].visible = visible;
    }
    SaveVisible();
    std::string list = JoinVisible();
    ESP_LOGI(TAG, "field %s -> %s (visible now: %s)", kFieldNames[idx].token,
             visible ? "shown" : "hidden", list.c_str());
    return list;
}

std::string atz_car_page_set_fields(const std::string& spec) {
    std::string lower;
    for (char ch : spec) {
        lower.push_back((char)tolower((unsigned char)ch));
    }
    bool want[kFieldCount] = {};
    bool all = false;
    if (lower == "all" || lower == "*" || lower == "全部" || lower == "所有" || lower.empty()) {
        all = true;
    } else if (lower == "none" || lower == "无" || lower == "都不要") {
        // 保持 want 全 false
    } else {
        size_t pos = 0;
        while (pos <= spec.size()) {
            size_t comma = spec.find(',', pos);
            std::string tok = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            const int idx = LookupField(tok);
            if (idx >= 0) {
                want[idx] = true;
            } else if (!tok.empty()) {
                ESP_LOGW(TAG, "ignoring unknown field '%s'", tok.c_str());
            }
            if (comma == std::string::npos) {
                break;
            }
            pos = comma + 1;
        }
    }
    if (g_display != nullptr) {
        DisplayLockGuard lock(g_display);
        for (int i = 0; i < kFieldCount; i++) {
            g_fields[i].visible = all || want[i];
        }
        ApplyFieldVisibility();
        RefreshPage();
    } else {
        for (int i = 0; i < kFieldCount; i++) {
            g_fields[i].visible = all || want[i];
        }
    }
    SaveVisible();
    std::string list = JoinVisible();
    ESP_LOGI(TAG, "fields '%s' -> visible: %s", spec.c_str(), list.c_str());
    return list;
}

std::string atz_car_page_get_fields(void) {
    return JoinVisible();
}

std::string atz_car_page_list_field_names(void) {
    std::string out;
    for (int i = 0; i < kFieldCount; i++) {
        if (!out.empty()) {
            out += ", ";
        }
        out += NameOf((FieldIndex)i);
        out += "(";
        out += kFieldNames[i].token;
        out += ")";
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════

void atz_car_page_init(Display* display) {
    if (display == nullptr || g_display != nullptr) {
        return;
    }
    g_display = display;
    LoadVisible();

    {
        DisplayLockGuard lock(g_display);
        BuildPage(lv_screen_active());
        ApplyFieldVisibility();
        RefreshPage();
    }

    g_timer = lv_timer_create(PageTimerCb, ATZ_CAR_PAGE_REFRESH_MS, nullptr);

    // ── 语音入口 ──────────────────────────────────────────────────────────
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool(
        "self.ui.show_car_page",
        "Show the full-screen vehicle dashboard page. Call this when the user asks to SEE the car "
        "data, e.g. \"显示车况\", \"看一下车况\", \"车况页面\", \"打开仪表\", \"看水温油温\", "
        "\"show car status\", \"open dashboard\".\n"
        "The page shows live values received from the car instrument over ESP-NOW: RPM, speed, "
        "coolant, oil temperature, intake air, engine load, throttle, battery voltage, oil pressure "
        "and air-fuel ratio, plus the link status. It stays on screen until the user asks to close "
        "it (`self.ui.hide_car_page`) or presses the BOOT button.\n"
        "The user can choose WHICH items are shown with `self.ui.set_car_page_field` and "
        "`self.ui.set_car_page_fields`.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            (void)properties;
            atz_car_page_show();
            return true;
        });

    mcp_server.AddTool(
        "self.ui.hide_car_page",
        "Close the vehicle dashboard page and go back to the normal face screen. Call this when the "
        "user says \"关掉车况\", \"返回\", \"退出车况页\", \"hide car page\", \"close dashboard\".",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            (void)properties;
            atz_car_page_hide();
            return true;
        });

    // ★ 按用户要求：车况页显示哪些项目可以**用语音开关**，页面会自动重排不留空位。
    mcp_server.AddTool(
        "self.ui.set_car_page_field",
        "Show or hide ONE item on the vehicle dashboard page (self.ui.show_car_page). The page "
        "re-layouts automatically, so hiding an item leaves no empty slot.\n"
        "`field` accepts English or Chinese: rpm/转速, speed/车速, coolant/水温, oil/油温, "
        "intake/进气, load/负荷, tps/节气门, volt/电压, oilpress/油压, afr/空燃比.\n"
        "Examples: \"我不想看进气\" -> field=\"intake\", visible=false. "
        "\"把水温显示出来\" -> field=\"coolant\", visible=true. "
        "Returns the list of items that are visible afterwards.",
        PropertyList({Property("field", kPropertyTypeString),
                      Property("visible", kPropertyTypeBoolean, true)}),
        [](const PropertyList& properties) -> ReturnValue {
            auto field = properties["field"].value<std::string>();
            bool visible = properties["visible"].value<bool>();
            return atz_car_page_set_field(field, visible);
        });

    mcp_server.AddTool(
        "self.ui.set_car_page_fields",
        "Set EXACTLY which items the vehicle dashboard page shows (everything else is hidden). "
        "Use this when the user lists several items at once, e.g. \"只看转速和水温\" -> "
        "fields=\"rpm,coolant\"; \"全部显示\" / \"恢复默认\" -> fields=\"all\".\n"
        "Accepted names: rpm/转速, speed/车速, coolant/水温, oil/油温, intake/进气, load/负荷, "
        "tps/节气门, volt/电压, oilpress/油压, afr/空燃比. Returns the visible list.",
        PropertyList({Property("fields", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            auto fields = properties["fields"].value<std::string>();
            return atz_car_page_set_fields(fields);
        });

    ESP_LOGI(TAG, "car page ready (voice: show/hide page, set_car_page_field(s)); visible: %s",
             JoinVisible().c_str());
}

void atz_car_page_show(void) {
    if (g_display == nullptr || g_page == nullptr) {
        return;
    }
    DisplayLockGuard lock(g_display);
    RefreshPage();                                  // 先填一次，避免闪一下 -- 
    lv_obj_remove_flag(g_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_page);
    atz_rpm_ring_set_visible(false);                 // 转速圈让位，别压在页面上
    ESP_LOGI(TAG, "car page shown");
}

void atz_car_page_hide(void) {
    if (g_display == nullptr || g_page == nullptr) {
        return;
    }
    DisplayLockGuard lock(g_display);
    lv_obj_add_flag(g_page, LV_OBJ_FLAG_HIDDEN);
    atz_rpm_ring_set_visible(true);
    ESP_LOGI(TAG, "car page hidden");
}

void atz_car_page_toggle(void) {
    if (atz_car_page_visible()) {
        atz_car_page_hide();
    } else {
        atz_car_page_show();
    }
}

bool atz_car_page_visible(void) {
    if (g_display == nullptr || g_page == nullptr) {
        return false;
    }
    DisplayLockGuard lock(g_display);
    return !lv_obj_has_flag(g_page, LV_OBJ_FLAG_HIDDEN);
}
