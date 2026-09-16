// atz_ui.h -- AtzGauge 主界面定制（主题包 + 车况条 + 语音控制）
//
// 全部实现都在板型目录里，**不改上游任何 UI 代码**：
//   * 主题包走上游现成的 LvglThemeManager::RegisterTheme()
//   * 车况条走「叠加式」覆写：先让上游把原版布局建好，我们再在它上面加一层
//
// 上游如果改了主界面布局，我们这层大概率照样生效（只依赖 screen / 主题对象，
// 不依赖上游那些控件的内部结构）。详见 MODIFICATIONS.md 的「升级后怎么办」。

#pragma once

#include "lcd_display.h"

#include <cstdint>

/**
 * 注册 AtzGauge 主题包，并把 NVS 里的主题默认值补成 ATZ_UI_DEFAULT_THEME。
 *
 * ★ 必须在 **构造显示对象之前** 调用。
 *   原因：LcdDisplay 的构造函数会立刻从 NVS 读上次的主题名并查表赋值给 current_theme_。
 *   如果 NVS 里存的是我们的主题名（atz-night 等）而此刻还没注册，查表得到 nullptr，
 *   后续 SetupUI() 解引用就会崩。
 */
void atz_ui_register_themes(void);

/**
 * AtzGauge 主显示类。
 * 继承上游 SpiLcdDisplay，只做两件事：叠加车况条、让车况条跟随主题换色。
 */
class AtzLcdDisplay : public SpiLcdDisplay {
public:
    AtzLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                        swap_xy) {}

    void SetupUI() override;
    void SetTheme(Theme* theme) override;

    /**
     * 表情名兜底。
     *
     * ★ 为什么需要：上游 application.cc:438 在启动时设的表情是 "robot_2"，而 noto 表情集里
     *   只有 21 个名字（neutral/happy/shocked/...），没有 robot_2 → SetEmotion() 查不到图，
     *   退回字体图标。实测那只有 40×39 px，而表情素材是 112~128 px —— 整屏看上去
     *   "像什么都没显示"。这里把查不到的名字映射成真实存在的表情。
     */
    void SetEmotion(const char* emotion) override;

    /**
     * 状态文字（时钟就走这里）：`SetStatus("22:44")`。
     *
     * ★ 为什么要覆写：上游把时钟和"正在连接…"这类文字放在**同一个标签**里。
     *   而我们想让时钟更大 —— 如果直接放大字号（transform_zoom），那是**位图拉伸**，
     *   实测会明显发虚。固件里其实已经有现成的 **真 30px 字体**
     *   （font_noto_sans_basic_30_4；字体组件的 CMakeLists 用 file(GLOB src 下所有 .c)，
     *   本来就编进来了，只是没被引用而被链接器裁掉），所以这里按内容切字体：
     *     时钟（HH:MM）→ 30px 真字体（清晰，= 20px 的 150%）
     *     其它状态文字 → 主题原字体（20px，中文要它）
     */
    void SetStatus(const char* status) override;

    /**
     * 接收资源系统下发的表情集。
     *
     * ★ 为什么必须自己接：上游 assets.cc 只把表情集挂到**按名字找到的** light/dark 两套
     *   内置主题上（assets.cc:353-356），自定义主题拿不到。结果 SetEmotion() 查不到图片，
     *   退回字体图标 —— 实测中间只有 40px 的小图标，而不是 128px 的表情素材，
     *   整屏看上去"像没显示"。
     */
    void SetEmojiCollection(std::shared_ptr<EmojiCollection> collection) override;

    /** 运行时开关车况条（会被 MCP 工具调用；写 NVS，重启后仍生效）。 */
    void SetCarBarEnabled(bool on);
    bool IsCarBarEnabled() const { return car_bar_enabled_; }

    /**
     * 运行时开关顶栏图标：which = "wifi" | "battery"。
     * 默认值来自 atz_ui_config.h 的 ATZ_SHOW_NETWORK_ICON / ATZ_SHOW_BATTERY_ICON；
     * 一旦用语音改过就写进 NVS，之后以 NVS 为准。内部自己加显示锁。
     */
    void SetStatusIconVisible(const char* which, bool on);
    bool StatusIconWanted(const char* which) const;

    /** 取上游字幕文本（供逐字贴弧字幕）；带显示锁。 */
    size_t CopyChatText(char* buf, size_t len);

    /** 显示/隐藏上游那条直线字幕栏（bottom_bar_）。 */
    void SetStockSubtitleVisible(bool visible);

    /** 重新套用尺寸 + 打日志（供 /size 端点这类外部调用；内部自己加显示锁） */
    void ReapplySizes();

    /**
     * 把**当前真实几何**写进 buf（供 /size 端点回显）。
     * 为什么不用"素材 128px × 百分比"去算：表情素材实际是 128×121，而且可见的黄色圆
     * 比这个框更小（PNG 有透明边距），算出来的数字会和外面对不上，容易误导。
     * 内部自己加显示锁。
     */
    void DescribeSizes(char* buf, size_t len);

private:
    lv_obj_t* car_bar_ = nullptr;
    lv_obj_t* car_label_ = nullptr;
    lv_timer_t* car_timer_ = nullptr;
    bool car_bar_enabled_ = true;
    char car_text_[96] = {0};

    /**
     * 尺寸微调：放大表情图、放大顶部时间/状态文字。
     * 只改上游已有控件的大小（不新建控件），因此**不受 ATZ_UI_ENABLE 影响、始终生效**。
     * SetupUI() 里调一次；每次 SetEmotion() 后也调（换表情图后要重新套用缩放）。
     */
    void ApplySizes();

    /** 把缩放后的真实尺寸打进串口日志（让"放大了多少"是可核对的数字） */
    void LogSizes();

    /**
     * 圆形屏布局修正：把上游"整宽 360"的顶栏/底栏收进圆的可视区内。
     * 依据是 2*sqrt(R²−dy²)，参数见 atz_ui_config.h 的 ATZ_ROUND_*。
     */
    void ApplyRoundScreenLayout();

    // 以下三个都要求调用者已持有 LVGL 锁
    void BuildCarBar();
    void ApplyCarBarStyle();
    void UpdateCarBar();
    static void CarBarTimerCb(lv_timer_t* timer);

    /** 按 NVS/宏的设置显示或隐藏顶栏图标（要求已持有显示锁） */
    void ApplyStatusIconVisibilities();

    /** 把当前主题的文字色套到屏幕外沿的转速圈上（要求已持有显示锁） */
    void ApplyRingTheme();
};

/**
 * 把当前车况格式化成车况条文本。无有效数据时写入占位符。
 * 单独导出：便于在没有屏幕的情况下用台架/日志验证格式逻辑。
 * @return 写入的字符数（不含结尾 '\0'）
 */
int atz_ui_format_car_bar(char* buf, unsigned len);

/**
 * 注册运行时控制工具（语音可调）：
 *   self.ui.set_theme   —— 切换主题（含 light/dark 与三套 atz-*）
 *   self.ui.set_car_bar —— 开/关主界面车况条
 */
void atz_ui_register_tools(AtzLcdDisplay* display);

/**
 * 台架预览：连续注入合成车况若干秒，让车况条显示出来（本机 HTTP 端点与语音工具共用）。
 * 数值是假的，仅供看界面用。
 */
void atz_ui_car_bar_demo(int seconds);

/**
 * 台架预览（**转速扫掠**）：以 10Hz 注入一条像真车的转速曲线
 * （怠速游车 → 拉转速 → 顶一下 → 松油门换挡 → 再拉 → 回怠速），8 秒一个循环，
 * 同时让车速/水温/油温/负荷/节气门/电压跟着动，这样转速圈、车况条、车况页都能看出"活着"。
 *
 * @param seconds        持续秒数（1~120）
 * @param allow_redline  true 时峰值拉到 7000 转（会越过 6500 的红线阈值，
 *                       **本地音频告警会真的响一声**，用于顺便验证告警链路）；
 *                       false 时峰值 6400 转（只到琥珀区，不触发告警）。
 */
void atz_ui_car_rev_sim(int seconds, bool allow_redline);

/**
 * 立刻停掉正在跑的台架注入（转速模拟 / 固定值预览）。
 * 用途：用户说"停"、或 PC 上访问 `/carbar?mode=stop`。
 * 停车后主表数据会在 2 秒内变"陈旧"，转速环自动归零变暗。
 */
void atz_ui_car_inject_stop(void);

// ── 尺寸微调（运行时可调，便于"试一个值→立刻看到"） ─────────────────────────
// 百分比基准：表情 100% = 上游 128px 素材；顶部时间 100% = 上游 20px 字号。
// 运行时可改（/size 端点），改完立即生效、不需要重刷固件。
void atz_ui_bind(AtzLcdDisplay* display);          // 板级创建显示对象后调用一次
void atz_ui_get_scales(int* emoji_pct, int* clock_pct);
void atz_ui_set_scales(int emoji_pct, int clock_pct);

/**
 * 把当前尺寸的**真实几何**写进 buf（供 /size 端点回显）。
 * 走 atz_ui_bind() 记下的那个显示对象；没绑定就写一句提示。
 * 为什么不用"素材 128px × 百分比"算：素材实际是 128×121，而且可见圆比素材框更小
 * （PNG 有透明边距），算出来的数字和外面对不上，容易让人以为缩放没生效。
 * 内部自己加显示锁，可以从任意任务调用。
 */
void atz_ui_describe_sizes(char* buf, size_t len);

/**
 * 取上游字幕（chat_message_label_）的当前文本，**拷进调用者的缓冲区**（线程安全）。
 * 逐字贴弧字幕用它来跟踪"AI 现在在说什么"。返回写入的字节数（0 = 没有字幕）。
 */
size_t atz_ui_copy_chat_text(char* buf, size_t len);

/**
 * 显示/隐藏**上游原来的直线字幕**（bottom_bar_）。
 * 开了逐字贴弧之后要把直线那条藏起来，否则两套字幕会同时出现。
 */
void atz_ui_set_stock_subtitle_visible(bool visible);
