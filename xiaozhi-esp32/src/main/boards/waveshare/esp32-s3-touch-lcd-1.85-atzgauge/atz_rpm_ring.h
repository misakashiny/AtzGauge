// atz_rpm_ring.h -- 屏幕外沿的转速圈（参考 obd_brz_gauge 的仪表外观）
//
// 两个控件，都贴在圆形屏的最外沿：
//   ① bezel：一圈几 px 宽的细圈（"白圈"，颜色跟随主题文字色）
//   ② arc  ：一圈跟着**转速**走的弧，5500 转起变琥珀、6500 转起变红
//
// 数据来源与车况条、车况页完全相同：ESP-NOW 主表的 obd_data_cache。
// 主表超过 ATZ_UI_STALE_MS（2s）没数据时，弧归零并变暗 —— 绝不拿过期值装作实时。
//
// 所有函数都可以从任意任务调用（内部自己加显示锁）。
#pragma once

#include <cstdint>

class Display;

/** 建圈。必须在显示对象的 SetupUI() 之后调用一次（UI 得先存在）。 */
void atz_rpm_ring_init(Display* display);

/** 整圈显示/隐藏（打开「车况」整屏页时藏起来，避免压在页面上）。 */
void atz_rpm_ring_set_visible(bool visible);
bool atz_rpm_ring_visible(void);

/**
 * 转速环总开关（**语音可控**）：写 NVS，重启后仍生效。
 * 关闭时环与红区刻度一起隐藏，插值也停掉（不占 CPU）。
 */
void atz_rpm_ring_set_enabled(bool on);
bool atz_rpm_ring_enabled(void);
/** 把开关写进 NVS（语音工具 / /ring 端点用；set_enabled 本身只改内存与显示）。 */
void atz_rpm_ring_save_enabled(bool on);

// ── 换挡提示灯：已整块删除（2026-09-16，用户要求）──────────────────────────
// 车上本来就有换挡灯，屏幕上再闪一个既分心又和"告警红"抢同一条细环。
// 原来的 set/save_shift_rpm 与 NVS 键 shift_rpm 都不复存在。

/** 跟随主题换色（fg_rgb = 圈与弧的基色，传 0xRRGGBB；底槽用同色低不透明度，深浅主题都好看）。 */
void atz_rpm_ring_apply_theme(uint32_t fg_rgb);

/** 立刻按当前车况刷一帧（测试/刚切回主界面时用，不必等定时器）。 */
void atz_rpm_ring_refresh(void);

// ── 环自己的计数（帧率/耗时/重绘像素统计在 atz_perf.h）──────────────────────
// calls = 环定时器调用次数；pushes = 其中真正改了控件（= 触发重绘）的次数。
void atz_rpm_ring_perf_read(uint32_t* calls, uint32_t* pushes);
