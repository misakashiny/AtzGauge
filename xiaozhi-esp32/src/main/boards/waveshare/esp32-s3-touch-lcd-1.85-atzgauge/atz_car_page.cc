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
#include "atz_hit_zone.h"
#include "atz_perf.h"
#include "atz_trip.h"
#include "atz_ui_config.h"
#include "display.h"
#include "espnow_link.h"
#include "lvgl_theme.h"
#include "mcp_server.h"
#include "obd_data_cache.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#define TAG "AtzCarPage"

LV_FONT_DECLARE(font_noto_sans_basic_30_4);   // 车况页两个主数值用（清晰的真 30px 字体）

// ── 可调参数 ────────────────────────────────────────────────────────────────
#define ATZ_CAR_PAGE_REFRESH_MS   300     // 刷新周期
#define ATZ_CAR_PAGE_STALE_MS     2000    // 超过这么久没数据 → 整页视为"无数据"
#define ATZ_CAR_PAGE_COL_W        232     // 内容列宽（★ 由圆屏公式解出来的：见 ATZ_CAR_COL_Y 的说明）
#define ATZ_CAR_PAGE_CELL_W       108     // 网格单元宽（两列 + 间距 8 = 232 = 列宽）
#define ATZ_CAR_PAGE_GRID_GAP     8       // 网格列间距
#define ATZ_CAR_PAGE_GRID_ROW_GAP 3       // 网格行间距（从 4 收到 3：为的是让整个内容列落在 y=27..315 内）

// ── 圆形屏"某个 y 处能放多宽"的唯一公式 ─────────────────────────────────────
// 半径 180、圆心 y=179.5 时，dy 处的可视宽度 ≈ 2*sqrt(180² - dy²)。
// ★ 这是本文件里所有"能不能放下"判断的依据，改排版时先用它算一遍再动手。
static inline int SafeWidthAt(int y) {
    const int dy = (y > 179) ? (y - 179) : (179 - y);   // 距屏幕中心行
    if (dy >= 180) {
        return 0;
    }
    const double w = 2.0 * sqrt((double)(180 * 180 - dy * dy));
    return (int)w;
}

// 车况页内容列（hero 行 + 字段网格）的顶部位置。
// ★ 这个 y 与列宽是**联立解出来的**，别单独改其中一个。
//   内容列固有高度 = hero 105 + 行距 8 + 网格(4 行 × 31 + 3×3 间距 = 133) = 246px。
//   要求整列**四个角**都在圆内（R=180，圆心 179.5），并留 ≥4px 余量：
//     列宽 216 → y 允许 41..73 ；232 → 48..66 ；240 → 51..63（窗口仅 13px，太脆）
//     列宽 280 → **无解** ✗（实测四角最远 206.7 —— 用户在屏幕上看到的就是这个）
//   最终 列宽 232 + y=56 → 内容占 56..301，最坏角距 174.2 ✓
#define ATZ_CAR_COL_Y             56

// 「行程」页（page1）
// ★★ 2026-09-18 重排（用户："两个按钮移到下面来、优化下UI排版、要美观"）。
//    新的信息层次（竖向）：**数据在上、操作在下** ——
//      y=30  时长行「已记录 0:06」（整页唯一的状态文字）
//      y=66..198  数据卡网格（2 列 × 4 行，108×31）
//      y=221 两个按钮（开始/暂停记录、清零重来，220×30）
//    为什么把状态缩短成一行：原来顶部是"记录中"+按钮两行，既挤又头重脚轻；
//      现在"在不在记录"直接由**按钮文字**表达（"开始记录"↔"暂停记录"），
//      按钮再染成绿/灰，一眼就能看出来，顶部只留时长这一行。
// ★ 几何判据同第一节（四角到圆心 <180，留 4px 余量）：
//      「已记录 0:06」约 130×31 → y ∈ 8..321 → 取 30
//      网格 220×130（4 行 31 + 3×2 间距）→ y ∈ 40..275 → 取 66..198
//      按钮行 220×30 → y ∈ 38..296 → 取 221..251
//    宽度核算（20px 字体：一个汉字 20px、一个数字≈11px）：
//      「里程」40 + 「3.5km」51 = 91 ｜ 「转速」40 + 「7100rpm」88 = 128 ✓（卡宽 132）
//      「车速」40 + 「120km/h」82 = 122 ✓ ｜ 「电压」40 + 「13.9V」62 = 102 ✓
//    ★ 卡宽必须 ≥132：早先试 106 宽时 "120km/h"/"7100rpm" 会越出卡片边界压到隔壁。
#define ATZ_TRIP_PAGE_CELL_W      132     // 卡宽（两列 132 + 间距 8 = 272）
#define ATZ_TRIP_PAGE_CELL_H      31      // 单行高：名字与数值同一行
#define ATZ_TRIP_PAGE_GRID_GAP    8       // 列间距
#define ATZ_TRIP_PAGE_GRID_ROW_GAP 4      // 行间距
#define ATZ_TRIP_PAGE_CARDS       8       // 卡数量（2 列 × 4 行）
//   几何（穷举四角求解，判据 ≤176）：网格 272×136 → y ∈ 68..156 → 取 76（76..211）
//                                  按钮行 272×30 → y ∈ 68..262 → 取 228（228..257）
//                                  时长行 180×31 → y ∈ 29..300 → 取 30
#define ATZ_TRIP_TIME_Y           30      // 时长行
#define ATZ_TRIP_TIME_H           31
#define ATZ_TRIP_BTN_Y            228     // ★ 按钮在**下面**（用户要求；数据在上、操作在下）
#define ATZ_TRIP_BTN_H            30      // 按钮高度（比原来 26 更厚实，也更好按）
#define ATZ_TRIP_BTN_W            132     // 单按钮宽（两个 132 + 间距 8 = 272）
#define ATZ_TRIP_COL_Y            76      // 数据网格

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

// ── 「行程」页（page1）──────────────────────────────────────────────────────
int g_page_no = 0;                  // 0 = 车况，1 = 行程
lv_obj_t* g_trip_col = nullptr;     // 行程页数据网格所在列（flex column）
lv_obj_t* g_trip_time = nullptr;    // 「已记录 0:06」（整页唯一的状态文字）
lv_obj_t* g_trip_btn_row = nullptr; // 底部两个按钮的容器（跟着页一起显示/隐藏）
lv_obj_t* g_trip_grid = nullptr;    // 数据卡网格（flex row wrap）
lv_obj_t* g_trip_status = nullptr;  // 操作结果提示（点按钮后回一句）
lv_obj_t* g_rec_btn = nullptr;
lv_obj_t* g_rec_btn_label = nullptr;
lv_obj_t* g_reset_btn = nullptr;
int64_t g_trip_status_until_us = 0; // 状态提示的显示截止时间

// 点击区 id（注册时记下来，翻页时按页启用/停用；-1 = 还没注册/注册失败）
int g_zone_rec = -1;         // 行程页「开始/暂停记录」
int g_zone_reset = -1;       // 行程页「清零重来」

struct TripCard {
    lv_obj_t* value = nullptr;
    lv_obj_t* unit = nullptr;
};
TripCard g_trip_cards[ATZ_TRIP_PAGE_CARDS];

// ★ 6 张卡（圆屏高度只放得下 3 行 × 2 列）：项目名 2 字 + 数值（含单位）同一行。
//   宽度核算（20px 字体，一个汉字=20px、一个数字≈11px）：
//     里程 40 + "0.0km" 51 = 91 ✓   水温 40 + "104C" 51 = 91 ✓
//     转速 40 + "7100rpm" 94 = 134 ✓  电压 40 + "13.9V" 62 = 102 ✓
//   数值在 142px 内、不会挤到隔壁列（8 张卡的双行版就是在这里翻车的，见文件开头的记录）。
struct TripCardDef {
    const char* name;
    const char* unit;
    uint32_t color;
};
const TripCardDef kTripCards[ATZ_TRIP_PAGE_CARDS] = {
    {"里程", "km",   ATZ_PAGE_VALUE},
    {"转速", "rpm",  ATZ_PAGE_RPM},
    {"车速", "km/h", ATZ_PAGE_SPEED},
    {"水温", "C",    ATZ_PAGE_WARN},
    {"油温", "C",    ATZ_PAGE_WARN},
    {"进气", "C",    ATZ_PAGE_VALUE},
    {"负荷", "%",    ATZ_PAGE_VALUE},
    {"电压", "V",    ATZ_PAGE_VALUE},
};

// 前置声明（两页的构建/刷新/切换/点击区互相调用，顺序上谁先谁后都行）
void RefreshPage(void);
void RefreshTripPage(void);
void BuildTripPage(lv_obj_t* parent);
void RegisterHitZones(void);
void ApplyZoneEnable(void);
void RegisterMainScreenGestures(void);
void SwipeNext(void* user);
void SwipePrev(void* user);
void SwipeExit(void* user);

/** 注册一块点击区并起名（名字会出现在 /touch 自检与日志里，便于确认"点到的是哪个按钮"）。 */
int RegisterZone(int x1, int y1, int x2, int y2, atz_hit_cb_t cb, const char* name) {
    const int id = atz_hit_zone_add(x1, y1, x2, y2, cb, nullptr);
    atz_hit_zone_set_name(id, name);
    return id;
}

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

// ★ 原来的 PlaceAt()（LV_ALIGN_CENTER 定位）已经**全部删除**：
//   CENTER 要按对象"当前高度"算偏移，而刚建好的对象高度还是 LV_SIZE_CONTENT(0)，
//   实测错位 100+ px（缺陷 #23）。现在统一用 LV_ALIGN_TOP_MID + 绝对 y。

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
    // ★ 圆屏约束：文本行盒的**四角**必须在圆内。y=15..45 / 宽 130 时最坏角距 176.7 < 179.5 ✓
    //   （这就是为什么顶部文字必须短：y 越小可用宽越窄）
    g_link_label = MakeLabel(g_page, "● 车况", ATZ_PAGE_BAD, nullptr);
    lv_obj_set_style_text_align(g_link_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_link_label, LV_ALIGN_TOP_MID, 0, 15);

    // ── 内容列（flex column；隐藏字段后自动收缩并保持居中）──────────────────
    // ★ 用 TOP_MID 定位而不是 CENTER：CENTER 依赖对象高度，而 flex 容器的高度要等布局
    //   算完才知道（缺陷 #23 就是这么错位 100+px 的）。这里 y=27 → 网格末行落在 y=258，
    //   而 y=258 处可用宽 285px ≥ 列宽 280px，四角全部在圆内。
    g_col = MakeBox(g_page);
    lv_obj_set_width(g_col, ATZ_CAR_PAGE_COL_W);
    lv_obj_set_height(g_col, LV_SIZE_CONTENT);
    lv_obj_align(g_col, LV_ALIGN_TOP_MID, 0, ATZ_CAR_COL_Y);
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


    BuildTripPage(g_page);   // 「行程」页（page1）：默认隐藏

    lv_obj_add_flag(g_page, LV_OBJ_FLAG_HIDDEN);   // 默认隐藏
    ESP_LOGI(TAG, "car page built (2 pages: car / trip)");
}

// ═══════════════════════════════════════════════════════════════════════════
// 「行程」页（page1）—— 行程统计与峰值保持
//
// 布局（360x360 圆屏）：
//        ● 记录中 0:12:34                  y=6    状态行（一眼看出在不在记录）
//      [ 暂停记录 ] [ 清零重来 ]           y=38   两个**真的能点**的按钮（各 120×26）
//      ┌────────────────────────────────┐
//      │  里程 1.9km │ 转速 7100rpm     │  6 张数据卡，2 列 × 3 行（142×50）
//      │  车速 120km/h │ 水温 104C      │
//      │  油温 118C  │ 电压 13.8V       │
//      └────────────────────────────────┘
//              ▴ 返回车况                    y=332  翻页按钮的提示（点击区在底部整条）
// ★ 顶部第一行**必须**能看出"到底在不在记录" —— 这是用户提出这个功能的原话
//   （"可以通过语音开始记录、重置记录"）：不在记录时是灰的"已暂停"，在记录时是绿的"记录中"。
// ★ 按钮"点不动"的根因与修法见 atz_hit_zone.h：本项目没给 LVGL 注册输入设备，
//   所以按钮的点击必须走"自己算命中"这条路（RegisterHitZones 就在下面）。
// ═══════════════════════════════════════════════════════════════════════════

void TipShow(const char* text) {
    if (g_trip_status == nullptr) {
        return;
    }
    lv_label_set_text(g_trip_status, text);
    g_trip_status_until_us = esp_timer_get_time() + 3000000;   // 显示 3 秒
}

// ── 按钮动作（与"怎么被触发"解耦：LVGL 点击事件 + 触摸命中区都调它）──────────
// ★ 注意：从触摸任务调进来时**不持显示锁**，所以这里自己加（DisplayLockGuard 可重入地
//   取决于实现，所以宁可只在一处加锁 —— 见 RefreshTripPage 的调用约定）。
void DoToggleRecord(void) {
    const bool now = !atz_trip_recording();
    atz_trip_set_recording(now);
    TipShow(now ? "已开始记录" : "已暂停记录");
    ESP_LOGI(TAG, "button: record -> %s", now ? "START" : "PAUSE");
}

void DoResetTrip(void) {
    atz_trip_reset();
    TipShow("已清零");
    ESP_LOGI(TAG, "button: trip reset");
}

// 触摸命中区的回调（跑在触摸任务上下文；user 不用）
void HitToggleRecord(void* user) {
    (void)user;
    if (g_display == nullptr) {
        return;
    }
    DisplayLockGuard lock(g_display);
    DoToggleRecord();
    RefreshTripPage();
}

void HitResetTrip(void* user) {
    (void)user;
    if (g_display == nullptr) {
        return;
    }
    DisplayLockGuard lock(g_display);
    DoResetTrip();
    RefreshTripPage();
}

// LVGL 点击回调：**当前不会触发**（没有输入设备），留着是为了将来若把触摸接进 LVGL
// 时这套按钮还能用，也让"按钮该干什么"只有一处定义。
void RecBtnCb(lv_event_t* e) {
    (void)e;
    DoToggleRecord();
    RefreshTripPage();
}

void ResetBtnCb(lv_event_t* e) {
    (void)e;
    DoResetTrip();
    RefreshTripPage();
}

void RefreshTripPage(void) {
    if (g_trip_col == nullptr) {
        return;
    }
    atz_trip_stats_t t = {};
    atz_trip_get(&t);
    const bool rec = atz_trip_recording();

    // ── 状态行：记录中 / 已暂停（第 1 行）+ 已记录时长（第 2 行）─────────────
    //    ★ 为什么拆成两行：圆形屏顶部 y=8..68 那一段可用宽度只有 ~250px，
    //      "● 记录中 1:23:45" 并成一行要 330px，**根本放不下**（实测算过）。
    //      拆开之后每行都只占 ~70px，稳稳在圆内。
    //    ★ 时长必须是"从按下开始记录起累计"的秒数（atz_trip 里累加），不是开机秒数。
    // ── 时长行（唯一的状态文字）────────────────────────────────────────────
    if (g_trip_time != nullptr) {
        char text[32];
        const uint32_t sec = t.uptime_s;
        snprintf(text, sizeof(text), "记录 %lu:%02lu:%02lu", (unsigned long)(sec / 3600),
                 (unsigned long)((sec / 60) % 60), (unsigned long)(sec % 60));
        if (strcmp(lv_label_get_text(g_trip_time), text) != 0) {
            lv_label_set_text(g_trip_time, text);
            atz_perf_count_push();
        }
        lv_obj_set_style_text_color(g_trip_time,
                                    lv_color_hex(rec ? ATZ_PAGE_VALUE : ATZ_PAGE_STALE), 0);
    }
    // 已记录时长：并入状态行（"记录中 0:03"）。★ 时长单独一行试过，会和按钮行贴住；
    // 放底部提示行也试过，但底部提示已经删除（翻页改用滑动）。合并后实测宽度 ~110px，
    // 远小于 y=2..64 那一段圆屏可用宽度。
    // ── 记录按钮：文字 + **底色**一起变（记录中 = 实心绿；暂停 = 深灰描边）──────
    if (g_rec_btn_label != nullptr && g_rec_btn != nullptr) {
        const char* want = rec ? "暂停记录" : "开始记录";
        if (strcmp(lv_label_get_text(g_rec_btn_label), want) != 0) {
            lv_label_set_text(g_rec_btn_label, want);
            if (rec) {
                lv_obj_set_style_bg_color(g_rec_btn, lv_color_hex(ATZ_PAGE_OK), 0);
                lv_obj_set_style_bg_opa(g_rec_btn, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(g_rec_btn, 0, 0);
                lv_obj_set_style_text_color(g_rec_btn_label, lv_color_hex(0x08130C), 0);
            } else {
                lv_obj_set_style_bg_color(g_rec_btn, lv_color_hex(0x1F262E), 0);
                lv_obj_set_style_bg_opa(g_rec_btn, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(g_rec_btn, 1, 0);
                lv_obj_set_style_border_color(g_rec_btn, lv_color_hex(ATZ_PAGE_OK), 0);
                lv_obj_set_style_text_color(g_rec_btn_label, lv_color_hex(ATZ_PAGE_OK), 0);
            }
        }
    }

    // ── 6 张数据卡 ─────────────────────────────────────────────────────────
    //  单位写法尽量短（"104C" 而不是 "104 C"），142px 的格子里才放得下名字 + 数值。
    char v[ATZ_TRIP_PAGE_CARDS][20];
    if (t.samples == 0) {
        // 没有任何有效样本：全部显示 --（不要显示 0，那会被误当成"真跑过"）
        for (int i = 0; i < ATZ_TRIP_PAGE_CARDS; i++) {
            snprintf(v[i], sizeof(v[i]), "--");
        }
    } else {
        // ★ 单位写法尽量短（"104C" 而不是 "104 C"），108px 的卡片里才放得下名字 + 数值
        snprintf(v[0], sizeof(v[0]), "%.1f%s", t.km_x100 / 100.0, kTripCards[0].unit);
        snprintf(v[1], sizeof(v[1]), "%u%s", (unsigned)t.max_rpm, kTripCards[1].unit);
        snprintf(v[2], sizeof(v[2]), "%u%s", (unsigned)t.max_speed, kTripCards[2].unit);
        snprintf(v[3], sizeof(v[3]), "%d%s", (int)t.max_coolant, kTripCards[3].unit);
        snprintf(v[4], sizeof(v[4]), "%d%s", (int)t.max_oil, kTripCards[4].unit);
        snprintf(v[5], sizeof(v[5]), "%d%s", (int)t.max_intake, kTripCards[5].unit);
        snprintf(v[6], sizeof(v[6]), "%d%s", (int)(t.max_load < 0 ? 0 : t.max_load),
                 kTripCards[6].unit);
        // 电压：min_bat_mv 是**毫伏**，必须除 1000（第一版直接印 13900.00 V，一眼假）
        snprintf(v[7], sizeof(v[7]), "%d.%d%s", (int)(t.min_bat_mv / 1000),
                 (int)((t.min_bat_mv % 1000) / 100), kTripCards[7].unit);
    }
    for (int i = 0; i < ATZ_TRIP_PAGE_CARDS; i++) {
        if (g_trip_cards[i].value == nullptr) {
            continue;
        }
        if (strcmp(lv_label_get_text(g_trip_cards[i].value), v[i]) != 0) {
            lv_label_set_text(g_trip_cards[i].value, v[i]);
            atz_perf_count_push();
        }
    }

    // ── 操作提示 3 秒后自动消失 ────────────────────────────────────────────
    if (g_trip_status != nullptr) {
        const bool show = (g_trip_status_until_us != 0) && (esp_timer_get_time() < g_trip_status_until_us);
        if (show) {
            lv_obj_remove_flag(g_trip_status, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_trip_status, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ApplyPageVisibility(void) {
    if (g_page == nullptr) {
        return;
    }
    ApplyZoneEnable();   // 点击区跟着页走（两页的底部提示条坐标相同，必须互斥启用）
    if (g_page_no == 1) {
        if (g_col != nullptr) lv_obj_add_flag(g_col, LV_OBJ_FLAG_HIDDEN);
        if (g_trip_col != nullptr) lv_obj_remove_flag(g_trip_col, LV_OBJ_FLAG_HIDDEN);
        if (g_trip_btn_row != nullptr) lv_obj_remove_flag(g_trip_btn_row, LV_OBJ_FLAG_HIDDEN);
        if (g_trip_time != nullptr) lv_obj_remove_flag(g_trip_time, LV_OBJ_FLAG_HIDDEN);
        // ★ 行程页**藏掉顶部那条"● 行程"**：它和本页自己的状态行（"● 记录中 0:00:02"）
        //   都挤在屏幕最上方，两行字挨在一起看着就是"重叠"（实测截图确认）。
        //   行程页的页名已由数据本身表达，顶部这条是多余的。
        if (g_link_label != nullptr) lv_obj_add_flag(g_link_label, LV_OBJ_FLAG_HIDDEN);
        RefreshTripPage();
    } else {
        // ★ 行程页的**每个顶层控件**都要藏（头部容器 + 数据网格 + 底部提示），否则会与
        //   车况页叠在一起（实测：翻回第 1 页后状态行/按钮/卡片还在，像两页糊在一起）。
        if (g_trip_col != nullptr) lv_obj_add_flag(g_trip_col, LV_OBJ_FLAG_HIDDEN);
        if (g_trip_btn_row != nullptr) lv_obj_add_flag(g_trip_btn_row, LV_OBJ_FLAG_HIDDEN);
        if (g_trip_time != nullptr) lv_obj_add_flag(g_trip_time, LV_OBJ_FLAG_HIDDEN);
        if (g_col != nullptr) lv_obj_remove_flag(g_col, LV_OBJ_FLAG_HIDDEN);
        if (g_link_label != nullptr) lv_obj_remove_flag(g_link_label, LV_OBJ_FLAG_HIDDEN);
        RefreshPage();   // 顶部链接状态与数值由它填
    }
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

// 行程页的数据卡（定义见文件开头的 kTripCards）。
// 卡片自己不做点击（点击交给整页：点一下翻回车况页），只有两个按钮是可点的。
/** 扁平的胶囊按钮：深底 + 细描边（圆屏上比默认主题按钮清爽，也不受主题色影响）。 */
void MakePill(lv_obj_t* btn, uint32_t border_rgb, int w, int h) {
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1F262E), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(border_rgb), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
}

/** 漂亮的胶囊按钮：**实心填充 + 圆角 + 无描边**（比"深底 + 细描边"更像真按钮）。 */
void MakePillFilled(lv_obj_t* btn, uint32_t bg_rgb, lv_opa_t bg_opa, int w, int h) {
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_rgb), 0);
    lv_obj_set_style_bg_opa(btn, bg_opa, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
}

void BuildTripPage(lv_obj_t* parent) {
    // ── ① 时长行（整页唯一的状态文字，y=30）────────────────────────────────
    g_trip_time = MakeLabel(parent, "记录 0:00:00", ATZ_PAGE_STALE, nullptr);
    lv_obj_set_style_text_align(g_trip_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_trip_time, LV_ALIGN_TOP_MID, 0, ATZ_TRIP_TIME_Y);

    // ── ② 数据网格（y=66 起，2 列 × 4 行）──────────────────────────────────
    g_trip_col = MakeBox(parent);
    lv_obj_set_width(g_trip_col, 2 * ATZ_TRIP_PAGE_CELL_W + ATZ_TRIP_PAGE_GRID_GAP);
    lv_obj_set_height(g_trip_col, LV_SIZE_CONTENT);
    // ★ TOP_MID（不是 CENTER）：CENTER 依赖对象高度，而高度此刻还是 LV_SIZE_CONTENT(0)，
    //   实测会被放到屏幕正中并与按钮叠在一起（缺陷 #23）。
    lv_obj_align(g_trip_col, LV_ALIGN_TOP_MID, 0, ATZ_TRIP_COL_Y);
    lv_obj_set_flex_flow(g_trip_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_trip_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_trip_col, ATZ_TRIP_PAGE_GRID_ROW_GAP, 0);

    // ── 数据卡网格 ────────────────────────────────────────────────────────
    g_trip_grid = MakeBox(g_trip_col);
    // 行程页网格：两列 168 + 间距 8 = 344（每张卡自己 168 宽，四角都在圆内）
    lv_obj_set_width(g_trip_grid, 2 * ATZ_TRIP_PAGE_CELL_W + ATZ_TRIP_PAGE_GRID_GAP);
    lv_obj_set_height(g_trip_grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_trip_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(g_trip_grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_trip_grid, ATZ_TRIP_PAGE_GRID_GAP, 0);
    lv_obj_set_style_pad_row(g_trip_grid, ATZ_TRIP_PAGE_GRID_ROW_GAP, 0);

    for (int i = 0; i < ATZ_TRIP_PAGE_CARDS; i++) {
        lv_obj_t* card = MakeBox(g_trip_grid);
        lv_obj_set_size(card, ATZ_TRIP_PAGE_CELL_W, ATZ_TRIP_PAGE_CELL_H);
        // 名字 + 数值同一行（168px 宽够放：「转速」40 + 「7100rpm」88 = 128 ✓）
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(card, 4, 0);

        MakeLabel(card, kTripCards[i].name, ATZ_PAGE_LABEL, nullptr);
        g_trip_cards[i].value = MakeLabel(card, "--", kTripCards[i].color, nullptr);
        g_trip_cards[i].unit = nullptr;   // 单位并进数值文本
    }

    // ── ③ 操作提示（点按钮后回一句；默认隐藏）──────────────────────────────
    g_trip_status = MakeLabel(parent, "", ATZ_PAGE_OK, nullptr);
    lv_obj_set_style_text_align(g_trip_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_trip_status, LV_ALIGN_TOP_MID, 0, ATZ_TRIP_BTN_Y - 34);
    lv_obj_add_flag(g_trip_status, LV_OBJ_FLAG_HIDDEN);

    // ── ④ 按钮行：**移到下面**（用户要求；数据在上、操作在下）────────────────
    //    不再用"头部容器"包住按钮 —— 它们现在挂在整页下方，翻页时和别的控件一起隐藏。
    lv_obj_t* btn_row = MakeBox(parent);
    lv_obj_set_width(btn_row, 2 * ATZ_TRIP_BTN_W + 8);
    lv_obj_set_height(btn_row, ATZ_TRIP_BTN_H);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 8, 0);
    lv_obj_align(btn_row, LV_ALIGN_TOP_MID, 0, ATZ_TRIP_BTN_Y);
    g_trip_btn_row = btn_row;

    // 记录按钮：**实心**（记录中 = 绿底深字；暂停 = 深灰底绿字）——
    // "在不在记录"靠底色一眼看出，不再需要单独一行"记录中/已暂停"文字。
    g_rec_btn = lv_button_create(btn_row);
    MakePillFilled(g_rec_btn, ATZ_PAGE_OK, LV_OPA_COVER, ATZ_TRIP_BTN_W, ATZ_TRIP_BTN_H);
    g_rec_btn_label = MakeLabel(g_rec_btn, "开始记录", 0x08130C, nullptr);
    lv_obj_center(g_rec_btn_label);
    lv_obj_add_event_cb(g_rec_btn, RecBtnCb, LV_EVENT_CLICKED, nullptr);

    // 清零按钮：描边式（次要操作，不跟主按钮抢眼）
    g_reset_btn = lv_button_create(btn_row);
    MakePill(g_reset_btn, ATZ_PAGE_LABEL, ATZ_TRIP_BTN_W, ATZ_TRIP_BTN_H);
    lv_obj_t* reset_label = MakeLabel(g_reset_btn, "清零重来", ATZ_PAGE_LABEL, nullptr);
    lv_obj_center(reset_label);
    lv_obj_add_event_cb(g_reset_btn, ResetBtnCb, LV_EVENT_CLICKED, nullptr);

    RegisterHitZones();
    lv_obj_add_flag(g_trip_col, LV_OBJ_FLAG_HIDDEN);   // 默认显示的是车况页
    lv_obj_add_flag(g_trip_time, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_trip_btn_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_trip_status, LV_OBJ_FLAG_HIDDEN);
}

// ═══════════════════════════════════════════════════════════════════════════
// 点击区注册（★ 让"按钮真的能点"的唯一办法，原因见 atz_hit_zone.h）
//
// 几何按上面的常量算出来，与控件位置一一对应；改控件位置时**必须同步改这里**，
// 否则会出现"按钮看着在 A、实际要点 B"这种最难查的问题。
// 矩形故意比按钮**大一圈**（左右 +6、上下 +4），手指偏一点也能按到。
//
// ★ 2026-09-18：翻页**不再用底部提示条点击区**，改成左右滑动（用户要求）。
//   原来的两个区（flip->trip / flip->car）已删除，底部那两行文字也一起删了。
// ═══════════════════════════════════════════════════════════════════════════

/** 滑动回调：向左划（←）= 进入下一页（车况 → 行程）。 */
void SwipeNext(void* user) {
    (void)user;
    ESP_LOGI(TAG, "swipe left -> trip page");
    atz_car_page_set_page(1);
}

/** 滑动回调：向右划（→）= 退回上一页（行程 → 车况）。 */
void SwipePrev(void* user) {
    (void)user;
    ESP_LOGI(TAG, "swipe right -> car page");
    atz_car_page_set_page(0);
}

/** 滑动回调：在车况页向右划（→）= **退出整屏页，回到主界面（表情页）**。 */
void SwipeExit(void* user) {
    (void)user;
    ESP_LOGI(TAG, "swipe right on car page -> back to main screen");
    atz_car_page_hide();
}

void RegisterHitZones(void) {
    // ── 行程页：两个按钮（位置由头部常量算出，与控件一一对应）────────────────
    {
        const int row_w = 2 * ATZ_TRIP_BTN_W + 8;
        const int row_x = (LV_HOR_RES - row_w) / 2;
        const int y1 = ATZ_TRIP_BTN_Y - 4;
        const int y2 = ATZ_TRIP_BTN_Y + ATZ_TRIP_BTN_H + 4;
        g_zone_rec = RegisterZone(row_x - 6, y1, row_x + ATZ_TRIP_BTN_W - 1 + 6, y2,
                                  HitToggleRecord, "trip.record");
        g_zone_reset = RegisterZone(row_x + ATZ_TRIP_BTN_W + 8 - 6, y1, row_x + row_w - 1 + 6, y2,
                                    HitResetTrip, "trip.reset");
    }

    ApplyZoneEnable();   // 初始显示车况页
}

/**
 * 按当前页启用/停用各自的点击区 + 注册滑动回调（**每次翻页时调用**）。
 *
 * 点击区：行程页的两个按钮只在行程页生效。
 * 滑动：
 *   · 车况页(page0)：左划 → 行程页(page1)；右划 → 不注册（已经是第一页）
 *   · 行程页(page1)：右划 → 车况页(page0)；左划 → 不注册（已经是第二页）
 *
 * ★★ 这里**只看 `g_page_no`，绝不看"页面当前是否可见"** —— 踩过的坑：
 *   `ApplyPageVisibility()` 是**先调本函数、最后才清 HIDDEN 标志**的，所以在
 *   show/set_page 的执行过程中 `atz_car_page_visible()` 还返回 false。
 *   早先这里写了 `if (!atz_car_page_visible()) { 清空 handler; return; }`，
 *   结果**每次翻页都把刚注册的滑动回调清掉** —— 实测表现就是"车况页左划不动"。
 *   "整屏页没显示"那种情况由 RegisterMainScreenGestures() 单独负责。
 */
void ApplyZoneEnable(void) {
    const bool trip = (g_page_no == 1);
    atz_hit_zone_set_enabled(g_zone_rec, trip);
    atz_hit_zone_set_enabled(g_zone_reset, trip);
    atz_swipe_set_handler(ATZ_SWIPE_LEFT, trip ? nullptr : SwipeNext, nullptr);
    // 右划：行程页 → 车况页；车况页 → **退回主界面（表情页）**（用户要求）。
    atz_swipe_set_handler(ATZ_SWIPE_RIGHT, trip ? SwipePrev : SwipeExit, nullptr);
}

/**
 * 整屏页**没显示**（= 主界面）时的手势归属：两个方向都不注册，
 * 于是手势落到触摸任务的兜底分支 —— **主界面左划 → 打开车况页**（用户要求）。
 * 在 atz_car_page_init() 与 atz_car_page_hide() 里调用。
 */
void RegisterMainScreenGestures(void) {
    atz_swipe_set_handler(ATZ_SWIPE_LEFT, nullptr, nullptr);
    atz_swipe_set_handler(ATZ_SWIPE_RIGHT, nullptr, nullptr);
}

void RefreshPage(void) {
    if (g_page == nullptr) {
        return;
    }
    const bool fresh = espnow_slave_has_data() &&
                       espnow_slave_last_rx_age_ms() >= 0 &&
                       espnow_slave_last_rx_age_ms() <= ATZ_CAR_PAGE_STALE_MS;

    // 顶部链路状态（★ 圆屏：文字要短，y=-150 处可用宽度约 198px）
    //   ★ 注意：行程页(page1) 时顶部那行归 ApplyPageVisibility() 管，这里**必须让开**，
    //     否则每 300ms 就被改回"车况 10Hz"，把"● 行程"覆盖掉（实测就是这个问题）。
    if (g_link_label != nullptr && g_page_no == 0) {
        // ★ 三种状态的文字都控制在 ~10 个字符内：这一行在 y=15..45，圆屏最窄处，
        //   宽度上限 176px（实测 "● 车况 无数据" = 130px ✓）。
        if (fresh) {
            lv_label_set_text(g_link_label, "● 车况 10Hz");
            lv_obj_set_style_text_color(g_link_label, lv_color_hex(ATZ_PAGE_OK), 0);
        } else if (espnow_slave_last_rx_age_ms() < 0) {
            lv_label_set_text(g_link_label, "● 未连接主表");
            lv_obj_set_style_text_color(g_link_label, lv_color_hex(ATZ_PAGE_BAD), 0);
        } else {
            lv_label_set_text(g_link_label, "● 主表已断流");
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
            atz_perf_count_push();                        // 供 /perf 观察"这一页到底改了多少次"
        }
    }

    // ★ 2026-09-18：原来底部那行「检查主表与信道」已删除 —— 它和顶部链路状态说的是同一件事
    //   （"无数据"），而且原位置正好压在上游**低电量弹窗**（y=309..339）那一带上，
    //   用户报过重叠。现在"无数据"只在顶部那一行表达（红字），底部不再有第二处提示。

    // 注：行程/峰值原来挤在这里的一行小字里（"峰值 … rpm / … C / … C"），
    // 2026-09-16 按用户要求搬到「行程」页（page1）单独显示 8 项，这里只剩翻页提示。
}

void PageTimerCb(lv_timer_t* timer) {
    // 跑在 LVGL 任务里，锁由 lvgl_port 持有，这里不能再取锁
    (void)timer;
    if (g_page == nullptr || lv_obj_has_flag(g_page, LV_OBJ_FLAG_HIDDEN)) {
        return;   // 页面没显示：一次控件都不碰（省电，也避免无意义重绘）
    }
    if (g_page_no == 1) {
        RefreshTripPage();
    } else {
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

    // 主界面手势兜底：左划进车况页（整屏页没显示时生效；翻页时由 ApplyZoneEnable 接管）
    RegisterMainScreenGestures();

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

    mcp_server.AddTool(
        "self.ui.show_trip_page",
        "Show the full-screen TRIP page (the second page of the car dashboard). It shows the trip "
        "statistics / peak-hold values recorded so far: estimated distance, maximum RPM, maximum "
        "speed, peak coolant / oil / intake temperature, peak engine load, minimum battery voltage, "
        "and how long the recording has been running, plus whether recording is ON or PAUSED.\n"
        "Call this when the user asks to SEE them, e.g. \"看行程统计\", \"打开行程页\", "
        "\"看一下峰值\", \"show trip page\", \"trip statistics\". "
        "Recording itself is controlled with `self.car.set_trip_recording`.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            (void)properties;
            atz_car_page_set_page(1);
            return true;
        });

    mcp_server.AddTool(
        "self.ui.set_car_page_page",
        "Switch the car dashboard between its two pages without closing it: "
        "page=0 -> live values (RPM, speed, temperatures...), page=1 -> trip statistics / peaks. "
        "Same as the user tapping the screen once.",
        PropertyList({Property("page", kPropertyTypeInteger, 0, 1)}),
        [](const PropertyList& properties) -> ReturnValue {
            atz_car_page_set_page(properties["page"].value<int>());
            return true;
        });

    ESP_LOGI(TAG, "car page ready (voice: show/hide page, set_car_page_field(s), show_trip_page); "
                  "visible: %s",
             JoinVisible().c_str());
}

void atz_car_page_show(void) {
    if (g_display == nullptr || g_page == nullptr) {
        return;
    }
    DisplayLockGuard lock(g_display);
    g_page_no = 0;                                   // 语音「显示车况」永远先给实时页
    ApplyPageVisibility();
    lv_obj_remove_flag(g_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_page);
    ESP_LOGI(TAG, "car page shown (page %d)", g_page_no);
}

void atz_car_page_hide(void) {
    if (g_display == nullptr || g_page == nullptr) {
        return;
    }
    DisplayLockGuard lock(g_display);
    lv_obj_add_flag(g_page, LV_OBJ_FLAG_HIDDEN);
    // 页面藏起来后：点击区失效、滑动权交回主界面兜底（左划 → 再进车况页）
    RegisterMainScreenGestures();
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

void atz_car_page_set_page(int page) {
    if (g_display == nullptr || g_page == nullptr) {
        return;
    }
    DisplayLockGuard lock(g_display);
    g_page_no = (page == 1) ? 1 : 0;
    ApplyPageVisibility();
    lv_obj_remove_flag(g_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_page);
    ESP_LOGI(TAG, "car page -> page %d (%s)", g_page_no, g_page_no == 1 ? "trip" : "car");
}

int atz_car_page_page(void) {
    return g_page_no;
}

void atz_car_page_flip(void) {
    atz_car_page_set_page(g_page_no == 0 ? 1 : 0);
}
