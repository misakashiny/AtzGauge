// atz_arc_text.h -- 字幕「逐字贴弧」（主界面底部）
//
// 背景：上游的字幕是一条**直线**贴在圆屏底部，圆屏底部有效宽度只有 ~180px，
// 一句话的两端必然被圆边切掉。这里把整句拆成单字，逐字摆在一段圆弧上并旋转到切线方向
// （笑脸形），文字就顺着屏幕边缘走 —— 同半径下比直线能多放 50% 以上的字。
//
// 实现要点见 atz_arc_text.cc 顶部注释。
#pragma once

class Display;

/** 建弧形字幕层（在显示对象 SetupUI() 之后调用一次）。 */
void atz_arc_text_init(Display* display);

/** 开关（关掉时把上游原来的直线字幕放回来）。 */
void atz_arc_text_set_enabled(bool on);
bool atz_arc_text_enabled(void);

/** 立刻按当前字幕文本重排一次（调试/刚切页面时用）。 */
void atz_arc_text_refresh(void);

/**
 * 主题变化后调用：字体对象换了，弧上的字要重新量宽重排。
 * （★ 教训：不要把字体指针缓存到控件上 —— 这个项目的字体是资源，换主题/资源刷新时会重建，
 *   缓存下来的指针会悬空，下一次排版就崩在 lv_font_get_glyph_width。）
 */
void atz_arc_text_on_theme_changed(void);
