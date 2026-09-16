// atz_dim.h -- 自动调光（夜间 / 空闲降亮）
//
// 只改**背光亮度**，不切主题、不动配色 —— 副作用最小。
// 参数在 atz_ui_config.h 的第 11 节（ATZ_DIM_*）。
#pragma once

class Display;

/** 建自动调光定时器（在显示对象 SetupUI() 之后调用一次；内部幂等）。 */
void atz_dim_init(Display* display);

/** 用户刚有动作（触摸/唤醒）→ 立刻恢复全亮并重新计时空闲。 */
void atz_dim_kick(void);
