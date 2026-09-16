// atz_car_page.h -- 「车况」整屏页面（语音进入）
//
// 需求：说一句"显示车况"就把整屏切到车况仪表页，显示 ESP-NOW 从主表收到的 10 个字段：
//       转速 / 车速 / 水温 / 油温 / 进气温度 / 负荷 / 节气门 / 电压 / 油压 / 空燃比
//
// 设计要点（为什么这么做）：
//   * **独立全屏层**，而不是改上游显示类：在 lv_screen_active() 上建一个整屏容器，
//     显示/隐藏它即可。上游的 emoji/label 全都不动，也就不可能再出"黑屏"那类事故。
//   * **不依赖 ATZ_UI_ENABLE**：主界面可以是官方原版，车况页照样能用（两者互不牵连）。
//   * 数据只在 lv_timer 里刷新（跑在 LVGL 任务上下文，锁由 lvgl_port 持有）；
//     从 HTTP/MCP 调进来的显示/隐藏会自己加锁。
//   * 数值有"新鲜度门控"：超过 ATZ_CAR_PAGE_STALE_MS 没收到主表数据，整页数值变灰并显示
//     "无数据"，绝不把过期值当实时值显示。

#pragma once

#include <string>

class Display;

/** 初始化：记住显示对象、建页面、注册语音工具。在板级 InitializeTools() 里调用。 */
void atz_car_page_init(Display* display);

/** 显示 / 隐藏 / 切换车况页（内部自己加显示锁，可从任意任务调用）。 */
void atz_car_page_show(void);
void atz_car_page_hide(void);
void atz_car_page_toggle(void);
bool atz_car_page_visible(void);

// ── 字段开关（语音「我不想看进气」→ 关掉这一项，页面自动重排） ──────────────
// 字段名中英文都认：rpm/转速、speed/车速、coolant/水温、oil/油温、intake/进气、
// load/负荷、tps/节气门、volt/电压、oilpress/油压、afr/空燃比。
// 三个函数都返回"改完之后可见的字段列表"（逗号分隔的英文 token），方便语音回复确认。

/** 打开/关闭其中一项。field 不认识时返回 "unknown field: …"。 */
std::string atz_car_page_set_field(const std::string& field, bool visible);

/** 只显示指定的几项（逗号分隔）；传 "all" 恢复全部。 */
std::string atz_car_page_set_fields(const std::string& spec);

/** 当前可见的字段列表（英文 token，逗号分隔）。 */
std::string atz_car_page_get_fields(void);

/** 全部字段名（中文(token) 形式），用于给大模型/用户看的说明。 */
std::string atz_car_page_list_field_names(void);
