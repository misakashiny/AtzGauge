// atz_hit_zone.h -- 触摸"点击区"注册表（给自绘页面用的按钮命中判定）
//
// ★ 为什么需要这个模块（2026-09-17 的根因）：
//   本项目**没有给 LVGL 注册任何输入设备**（`lv_indev_create` 全工程零处）——
//   触摸是板级 `TouchPollTask` 自己轮询 CST816S 的 5 字节寄存器读到的（见板级 .cc 顶部
//   的长注释：不能交给 esp_lvgl_port，因为它对每次读都 ESP_ERROR_CHECK，而休眠的
//   CST816S 会 NACK → 开机重启循环）。
//   后果：**`lv_button` 的 `LV_EVENT_CLICKED` 永远不会触发** —— 按钮画出来了、看着能点，
//   但没有任何人把坐标喂给 LVGL。用户报的"两个按钮貌似也点不动"就是这个。
//
//   所以点击判定走这条链路：
//       TouchPollTask（板级，30ms 轮询，已经拿到 x/y）
//         → atz_hit_zone_test(x, y)    命中哪个区
//         → 区的回调（跑在触摸任务上下文，**不持 LVGL 锁**）
//         → 回调内部自己加显示锁改 UI
//
//   为什么不让 LVGL 管：只有把触摸交给 LVGL indev 才能用 lv_button 的点击事件，而那正是
//   踩过的坑（NACK → reboot loop）。命中判定自己写只要 20 行，还更好调（能 log 命中的矩形）。
//
// 用法：页面初始化时 `atz_hit_zone_add(...)` 注册若干个矩形，注册是持久的（不清除）。
//   坐标用**屏幕绝对像素**（左上角原点），与 `/layout` 输出的一致。
#pragma once

#include <stdint.h>

/** 命中区的回调（跑在触摸任务上下文；不要假设持有 LVGL 锁，需要就自己 DisplayLockGuard）。 */
typedef void (*atz_hit_cb_t)(void* user);

/** 注册一个点击区。rect = 屏幕绝对坐标；重复注册同一块区域时覆盖旧的那个。
 *  @return 区 id（>=0），失败（没空位/参数非法）返回 -1。 */
int atz_hit_zone_add(int x1, int y1, int x2, int y2, atz_hit_cb_t cb, void* user);

/**
 * 命中判定：返回被点中的区的 id（`atz_hit_zone_add` 的返回值），没命中返回 -1。
 * 只做判定，**不触发回调** —— 由调用方决定"确认几拍之后再触发"。
 */
int atz_hit_zone_test(int x, int y);

/** 触发某个区的回调（id 由 test 返回）。 */
void atz_hit_zone_fire(int id);

/** 已注册的区数量（给 /touch 自检报告用）。 */
int atz_hit_zone_count(void);

/** 第 i 个区的矩形与名字（名字可为空串）；越界返回 false。给 /touch 自检报告用。 */
bool atz_hit_zone_info(int i, int* x1, int* y1, int* x2, int* y2, const char** name);

/** 给区起个名（便于 /touch 日志里看出"点到的是哪个按钮"）；id 非法时忽略。 */
void atz_hit_zone_set_name(int id, const char* name);

/**
 * 启用/停用某个区。**停用的区不参与命中判定** —— 用于"两块区域位置相同、但只在各自页面
 * 有效"的情况：车况页底部提示条和行程页底部提示条坐标完全一样，靠这个开关区分。
 */
void atz_hit_zone_set_enabled(int id, bool enabled);
bool atz_hit_zone_enabled(int id);
