// atz_touch.h -- 触摸面板的台架自检接口
//
// 为什么单独抽出来：触摸控制器（CST816S）不应答 I2C 时，"点屏幕没反应"这句话
// 无法区分下面两种情况：
//   ① 固件逻辑问题（读到了触摸但没触发对话）
//   ② 面板根本没应答（固件拿不到任何触摸数据）
// 只有把"面板到底答不答"变成可观测的，才能定性。这个接口就是干这个的：
// 监听一段时间，统计读到多少次有效样本、最大手指数。
//
// 用法（设备联网后）：
//   http://<设备IP>:8099/touch        默认监听 5 秒
//   http://<设备IP>:8099/touch?seconds=15
// 打开页面后点屏幕，页面会告诉你：TOUCH DETECTED / chip answering but no touch /
// chip not answering。

#pragma once

#include <stddef.h>

/**
 * 监听 seconds 秒，把结论写进 out。
 * @return true 表示这段时间内确实读到了手指（触摸面板工作正常）
 */
bool atz_touch_watch(int seconds, char* out, size_t out_len);
