// atz_perf.h -- 显示层性能统计（帧率 / 单帧耗时 / 重绘像素）
//
// 为什么单独一个模块：这套统计原来寄生在**转速环**里，而转速环已经被用户关掉了
// （`ATZ_RPM_RING_ENABLE=0`）。统计本身是"显示层"的事，跟具体哪个控件无关，
// 所以搬出来独立存在 —— 否则关掉环之后 `/perf` 会静默变成全 0，等于工具坏了。
//
// 数据来源：LVGL 显示器事件（RENDER_START/READY），以及显示器的失效区域数组。
#pragma once

#include <stdint.h>

class Display;

/** 注册显示事件回调（在显示对象就绪后调用一次；内部幂等）。 */
void atz_perf_init(Display* display);

/** 手动累加一次"控件真的改了"的计数（供各 UI 模块上报自己的重绘原因）。 */
void atz_perf_count_push(void);

/** 读累计值（frames/busy_us/pushes 可为 nullptr）。 */
void atz_perf_read(uint32_t* frames, uint64_t* busy_us, uint32_t* pushes);

/** 读扩展值：worst_us = 最慢一帧，inv_px = 累计重绘像素，inv_max = 单帧峰值像素。 */
void atz_perf_ext(uint64_t* worst_us, uint64_t* inv_px, uint32_t* inv_max);
