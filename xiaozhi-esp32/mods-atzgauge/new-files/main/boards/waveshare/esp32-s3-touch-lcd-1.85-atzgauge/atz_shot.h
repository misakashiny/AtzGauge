// atz_shot.h -- 台架调试用：把当前屏幕截成 JPEG，通过局域网 HTTP 取回
//
// 为什么需要它：这个板子没有摄像头，PC 也没法"看"屏幕。上游其实已经实现了
// Display::SnapshotToJpeg()（LVGL 快照 → JPEG，CONFIG_LV_USE_SNAPSHOT=y），
// 只是唯一的出口是 MCP 工具 self.screen.snapshot —— 那要经过云端大模型才会被调用。
// 这里开一个只读的本地端点，让 PC 上的浏览器/脚本随时能抓一张图：
//
//     http://<设备IP>:8099/shot.jpg            截图（image/jpeg）
//     http://<设备IP>:8099/health             探活（text/plain，返回 ok）
//     http://<设备IP>:8099/theme?name=atz-day 切主题（等价于语音工具 self.ui.set_theme）
//     http://<设备IP>:8099/carbar?seconds=15  车况条预览（注入合成数据，仅供看界面）
//
// 安全考虑：只提供只读 GET；/theme 与 /carbar 会改变设备状态，但仅限台架用途，
// 不涉及网络配置或固件。默认开启，不想要就把 atz_ui_config.h 里的
// ATZ_UI_SHOT_SERVER_ENABLE 改成 0。

#pragma once

class Display;

/**
 * 启动本地截图端点。需要在显示对象创建之后调用。
 * 失败只打印警告，不影响其它功能。
 */
void atz_shot_server_start(Display* display);

/** 打印当前 UI 状态（主题名、控件是否存在），便于只靠串口日志排查 */
void atz_shot_log_ui_state(Display* display);
