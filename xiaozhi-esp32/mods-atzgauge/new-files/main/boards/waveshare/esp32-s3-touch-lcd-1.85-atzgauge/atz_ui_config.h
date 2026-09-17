// atz_ui_config.h -- AtzGauge 主界面「唯一调参面板」
//
// 设计目标：主界面的每一项可调参数都集中在这一个文件里。
//   改外观 = 改这里 + 重新编译，**不需要碰上游 xiaozhi 的任何文件**。
//   （本文件所在的板型目录由 main/CMakeLists.txt 的 file(GLOB BOARD_SOURCES ...) 自动收集，
//     所以新增 UI 文件也不会给上游 diff 增加负担。见 MODIFICATIONS.md。）
//
// 换主题的两种方式：
//   1) 运行时不重编译：对小智说「换成夜间模式」→ MCP 工具 self.ui.set_theme
//      （内部走 Display::SetTheme()，会写入 NVS，重启后仍然生效）
//   2) 编译期默认值：改下面的 ATZ_UI_DEFAULT_THEME
//
// 车况条的开关也有两种：
//   编译期 = ATZ_UI_CAR_BAR_ENABLE；运行时 = 对小智说「把车况条关掉」→ self.ui.set_car_bar
//
// 颜色一律写 0xRRGGBB，运行时由 lv_color_hex() 转换。

#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// 0.0.1 ★ 本机私密覆盖：atz_local.h（**必须在所有默认值之前**）
//     为什么放最前面：下面那些默认值都写成 `#ifndef X / #define X 默认值 / #endif`，
//     只有在**看到默认值之前**先把私密头 include 进来，覆盖才生效。
//     （教训：一开始把 include 放在文件末尾，结果成了"先定义后 include"，
//       编译器报 'ATZ_DEBUG_TOKEN' redefined [-Werror]，构建直接失败。）
//     这个文件不进 git：口令等私密值写在里面，仓库里只留公开占位值。
//     模板见 开发参考/atz_local.h.example。
// ═══════════════════════════════════════════════════════════════════════════
#if defined(__has_include)
#  if __has_include("atz_local.h")
#    include "atz_local.h"
#  endif
#endif

// ═══════════════════════════════════════════════════════════════════════════
// 0.0 固件标识（★ 每次启动打一条，/health 也回它）
//     为什么要有：台架上"现在设备里跑的到底是哪一版"以前只能靠比对日志猜。
//     现在串口启动日志和 http://<IP>:8099/health 都能直接看到构建时间与关键开关。
//     改完重要功能顺手把 ATZ_FW_STAGE 往前推一格（人读用）。
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_FW_NAME                 "atzgauge"
#define ATZ_FW_STAGE                "stage13-trip-page"   // 版本阶段名（**保持 ASCII**：/health 是机器读的，中文经 HTTP 易乱码）
#define ATZ_FW_BUILT                __DATE__ " " __TIME__     // 编译器填的构建时间

// ═══════════════════════════════════════════════════════════════════════════
// 0. ★ 总开关：主界面定制
//    0 = 完全用官方原版界面（SpiLcdDisplay + 上游主题与布局），本文件其余配置全部失效；
//    1 = 启用 AtzGauge 定制界面（主题包 + 车况条 + 语音控制）。
//
//    为什么留这个开关：定制界面出问题时，把它改成 0 重新编译就能立刻回到官方原版，
//    不必回滚代码。
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_UI_ENABLE               0

// ═══════════════════════════════════════════════════════════════════════════
// 1. 主题包
//    「主题」= 一组颜色 + 字体。上游已经内建 light / dark 两套，
//    这里再注册三套适合车机的。想加第四套：复制一段颜色定义，
//    再到 atz_ui.cc 的 BuildAmberTheme() 旁边照抄一个构建函数即可。
// ═══════════════════════════════════════════════════════════════════════════

// 出厂默认主题（仅当 NVS 里没有存过主题时才用得上；一旦用户切过主题，以 NVS 为准）
#define ATZ_UI_DEFAULT_THEME        "atz-night"

// ── atz-night · 座舱夜间（默认）：近黑底 + 中性白字，夜里不刺眼 ──────────────
#define ATZ_UI_NIGHT_BG             0x0B0F14   // 背景
#define ATZ_UI_NIGHT_TEXT           0xE6E6E6   // 主文字
#define ATZ_UI_NIGHT_CHAT_BG        0x151B22   // 聊天区背景
#define ATZ_UI_NIGHT_BORDER         0x2A323C   // 描边
#define ATZ_UI_NIGHT_LOW_BATTERY    0xFF5A5A   // 低电量提示
#define ATZ_UI_NIGHT_USER_BUBBLE    0x2E7D5B   // 用户气泡（WeChat 样式才用得到）
#define ATZ_UI_NIGHT_ASSIST_BUBBLE  0x1F262E   // 助手气泡
#define ATZ_UI_NIGHT_SYS_BUBBLE     0x0B0F14   // 系统气泡
#define ATZ_UI_NIGHT_SYS_TEXT       0x9FB3C8   // 系统文字

// ── atz-day · 白天高对比：浅底黑字，强光下最清楚 ────────────────────────────
#define ATZ_UI_DAY_BG               0xF2F4F7
#define ATZ_UI_DAY_TEXT             0x101418
#define ATZ_UI_DAY_CHAT_BG          0xE2E6EB
#define ATZ_UI_DAY_BORDER           0x8A939E
#define ATZ_UI_DAY_LOW_BATTERY      0xC62828
#define ATZ_UI_DAY_USER_BUBBLE      0xBFE6C8
#define ATZ_UI_DAY_ASSIST_BUBBLE    0xD8DEE5
#define ATZ_UI_DAY_SYS_BUBBLE       0xF2F4F7
#define ATZ_UI_DAY_SYS_TEXT         0x3A4552

// ── atz-amber · 琥珀夜间：黑底琥珀字，夜间最不刺眼的一种 ────────────────────
#define ATZ_UI_AMBER_BG             0x000000
#define ATZ_UI_AMBER_TEXT           0xFFB000
#define ATZ_UI_AMBER_CHAT_BG        0x140E00
#define ATZ_UI_AMBER_BORDER         0x6B4A00
#define ATZ_UI_AMBER_LOW_BATTERY    0xFF3B30
#define ATZ_UI_AMBER_USER_BUBBLE    0x4A3200
#define ATZ_UI_AMBER_ASSIST_BUBBLE  0x231700
#define ATZ_UI_AMBER_SYS_BUBBLE     0x000000
#define ATZ_UI_AMBER_SYS_TEXT       0xCC8A00

// 主题之间的间距倍率由上游 LvglTheme 内部固定为 2，没有公开 setter，
// 所以这里不提供该项（配置项宁可少，也不要留一个改了没用的假开关）。

// ═══════════════════════════════════════════════════════════════════════════
// 2. 主界面「车况条」
//    把 obd_data_cache 里的实时车况常显在表情下方。数据来自 ESP-NOW 主表。
//    没有任何数据时（没收到过包 / 超过 ATZ_UI_STALE_MS）整条隐藏，绝不显示假数据。
// ═══════════════════════════════════════════════════════════════════════════

#define ATZ_UI_CAR_BAR_ENABLE       1      // 编译期总开关：0 = 完全不创建车况条
#define ATZ_UI_CAR_BAR_WIDTH        210    // 宽（px）。圆屏安全上限约 226，别超过
#define ATZ_UI_CAR_BAR_BOTTOM_GAP   54     // 距屏幕底边（px）。字幕栏实测高 44px（y=316..359），
                                           // 所以必须 ≥ 46 才不会贴住；54 留 10px 呼吸空间。
#define ATZ_UI_CAR_BAR_RADIUS       16     // 圆角；0 = 方角
#define ATZ_UI_CAR_BAR_BG_OPA       40     // 底色不透明度 0~100（0 = 全透明，只留文字）
#define ATZ_UI_CAR_BAR_PAD          6      // 内边距（px）
#define ATZ_UI_CAR_BAR_BORDER_OPA   30     // 描边不透明度 0~100
#define ATZ_UI_CAR_BAR_UPDATE_MS    500    // 刷新周期（ms）
#define ATZ_UI_STALE_MS             2000   // 超过这么久没收到主表数据 → 整条隐藏

// 显示哪些字段（1 = 显示）。全部为 0 时车况条自动不创建。
#define ATZ_UI_CAR_BAR_SHOW_RPM     1
#define ATZ_UI_CAR_BAR_SHOW_SPEED   1
#define ATZ_UI_CAR_BAR_SHOW_COOLANT 1
#define ATZ_UI_CAR_BAR_SHOW_BATTERY 1
#define ATZ_UI_CAR_BAR_SHOW_OIL     0      // 油温：主表未上报时是 -100，默认关
#define ATZ_UI_CAR_BAR_SHOW_LOAD    0      // 发动机负荷 %
#define ATZ_UI_CAR_BAR_SHOW_TPS     0      // 节气门开度 %

// 字段单位与文案（想改成中文/其他写法就改这里）
#define ATZ_UI_UNIT_RPM             "rpm"
#define ATZ_UI_UNIT_SPEED           "km/h"
#define ATZ_UI_UNIT_TEMP            "C"
#define ATZ_UI_UNIT_VOLT            "V"
#define ATZ_UI_UNIT_PERCENT         "%"
#define ATZ_UI_NO_DATA_TEXT         "--"

// 表情名兜底：上游启动时设的是 "robot_2"，而 noto 表情集里没有这个名字（只有 21 个：
// angry/confident/confused/cool/crying/delicious/embarrassed/funny/happy/kissy/laughing/
// loving/neutral/relaxed/sad/shocked/silly/sleepy/surprised/thinking/winking）。
// 查不到时用下面这个，否则会退回 40px 的字体图标，看着像"屏幕没显示"。
#define ATZ_UI_EMOTION_FALLBACK     "neutral"

// ═══════════════════════════════════════════════════════════════════════════
// 2.5 尺寸微调（**不受 ATZ_UI_ENABLE 影响，始终生效**）
//     这两项只改"上游已有控件的大小"，不新建任何控件，所以风险最低。
//     100 = 保持上游原样；>100 = 放大；0 = 该项不处理。
//
//     换算基准：
//       * 表情图：上游素材 128×128 px（noto-color-emoji_128）→ lv_image_set_scale()
//       * 顶部时间：就是 status_label_（时钟走 SetStatus()），上游字号 20 px → transform_zoom
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_EMOJI_SCALE_PCT         125   // 表情图：128px → 160px
#define ATZ_CLOCK_SCALE_PCT         140   // 顶部时间：140 及以上用真 30px 字体（清晰），低于 125 用 20px

// ═══════════════════════════════════════════════════════════════════════════
// 3.6 圆形屏可用区（**关键几何**）
//
// 屏幕是圆的：设半径 R、离屏幕中心 dy 的地方，真正能看见的宽度只有
//        2 * sqrt(R² − dy²)
// R 取 175（留 5px 边框余量）。实测出来的可用宽度：
//
//     距中心   0px → 350px        距中心 140px → 210px (x 75..285)
//     距中心  60px → 329px        距中心 150px → 180px (x 90..270)
//     距中心 100px → 287px        距中心 160px → 142px (x 109..251)
//     距中心 120px → 255px        距中心 170px →  83px (x 138..222)
//
// ★ 上游的布局是"整宽 360 的顶栏/底栏" —— 在方屏上没问题，在圆屏上两端的元素
//   直接落到圆外（实测 WiFi 图标在 x=8..27，完全看不见）。下面这些值就是把它
//   拉回可用区里。
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_ROUND_ENABLE             1     // 0 = 用上游原始布局（方屏行为）
// 顶部排成"一行三件套"：[WiFi 图标] [时钟/状态] [电量图标]，三者都落在圆内。
// 依据：y=28..70 这一段的可用宽度约 180px（dy≈150 时 band 为 x90..270）。
#define ATZ_ROUND_TOP_BAR_WIDTH      180   // 顶栏宽（居中）
#define ATZ_ROUND_TOP_BAR_Y          28    // 顶栏距顶（下移，避开圆最窄的顶边）
#define ATZ_ROUND_STATUS_WIDTH       180   // 状态/时钟文字宽度（正好夹在两个图标中间）
#define ATZ_ROUND_STATUS_Y           28    // 与图标同一行，水平方向互不重叠
#define ATZ_ROUND_SUBTITLE_WIDTH     160   // 字幕文字宽度上限（原本是为了躲开转速环内缘半径 154；环已关，留窄一点更好看）
#define ATZ_ROUND_SUBTITLE_Y_OFFSET  36    // 字幕栏整体上移（原本是给底部转速环让位；环已关，位置保持用户认可的现状）

// ═══════════════════════════════════════════════════════════════════════════
// 3. 运行时持久化
//    运行时开关（车况条开关）存在 NVS 的哪个命名空间 / 键
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_UI_NVS_NAMESPACE        "atz_ui"
#define ATZ_UI_NVS_KEY_CAR_BAR      "car_bar"
#define ATZ_UI_NVS_KEY_ICON_WIFI    "icon_wifi"      // 顶栏 WiFi 图标是否显示（0/1）
#define ATZ_UI_NVS_KEY_ICON_BATTERY "icon_batt"      // 顶栏电量图标是否显示（0/1）
#define ATZ_UI_NVS_KEY_PAGE_FIELDS  "page_fields"    // 「车况」页显示哪些项目（逗号分隔的 token）
#define ATZ_UI_NVS_KEY_RPM_RING     "rpm_ring"       // 转速环开关（0/1，语音可改）
// 注：换挡提示灯的 NVS 键 "shift_rpm" 已随功能一起删除（2026-09-16）。老设备 NVS 里
//     可能还留着这个键，没有代码读它，无害；下次擦除 NVS 就没了。
// 一次性迁移标记：老设备 NVS 里存的是上游默认的 "light"（白底）。
// 第一次跑本 UI 代码时把主题改成 ATZ_UI_DEFAULT_THEME 并置位此标记；
// 之后用户自己切过的主题一律尊重，不再覆盖。
#define ATZ_UI_NVS_KEY_MIGRATED     "ui_migrated"

// ═══════════════════════════════════════════════════════════════════════════
// 4. 台架调试：本地截图端点
//    板子没有摄像头、PC 也看不到屏幕，所以开一个只读端点把屏幕截成 JPEG：
//        http://<设备IP>:8099/shot.jpg    截图    （启动日志里会打印完整地址）
//        http://<设备IP>:8099/health      探活
//    只提供两个只读 GET，不接受上传、不写状态。不想要就置 0。
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_UI_SHOT_SERVER_ENABLE   1
#define ATZ_UI_SHOT_SERVER_PORT     8099
#define ATZ_UI_SHOT_JPEG_QUALITY    85

// ═══════════════════════════════════════════════════════════════════════════
// 5. 顶栏图标（**不受 ATZ_UI_ENABLE 影响，始终生效**）
//    WiFi 图标 = 上游的 network_label_。圆屏顶栏本来就窄，图标挤在时钟左边很占地方，
//    所以默认关掉。运行时也能改：对小智说「把 WiFi 图标打开」→ MCP 工具 self.ui.set_status_icon
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_SHOW_NETWORK_ICON       0      // 0 = 不显示 WiFi 图标（默认）；1 = 显示
#define ATZ_SHOW_BATTERY_ICON       1      // 0 = 不显示电量图标

// ═══════════════════════════════════════════════════════════════════════════
// 6. 转速外圈（**不受 ATZ_UI_ENABLE 影响，始终生效**）——**已关闭**
//
//    2026-09-16 用户实测后要求："换挡提示灯删除掉吧、转速环也删除掉"。
//    所以 ATZ_RPM_RING_ENABLE 置 0：启动时不再新建环控件，代码整块编译期消失。
//    （环的"视觉告警"也跟着没了 —— 告警仍有蜂鸣，且车况页会把超限数值标红。）
//
//    下面是原设计与几何，留档：以后想要环（比如换成只在赛道模式显示）改回 1 即可。
//    ★ 按 obd_brz_gauge 的转速页设计（参考 src/screens/ui_ScreenPageRpm.c）：
//        ① 屏幕最外沿一圈**细环**（bezel）—— ui_helpers_create_ring(page, 10)
//        ② 环**内侧**一圈转速弧：lv_arc 340×340 / arc_width 20 / 直角 / 实心底槽
//        ③ 底槽是暗色，指示弧用主题强调色；不画旋钮
//      几何（360×360 圆屏，LVGL 的弧从半径往内画）：
//        bezel: 直径 360、线宽 10 → 占半径 170~180（外缘正好在屏幕边，无空隙）
//        arc  : 直径 340、线宽 20 → 占半径 150~170（紧贴 bezel 内侧，不相交）
//    ★ 两个对象都必须带 LV_OBJ_FLAG_FLOATING，否则会把屏幕撑出滚动条（见缺陷 #21）。
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_RPM_RING_ENABLE         0      // 总开关（运行时也能用语音/端点改，存 NVS）

// ── ① 外沿细环（obd_brz_gauge 的 create_ring(page, 10)）────────────────────
#define ATZ_RPM_RING_BEZEL          1      // 0 = 不画最外沿细环
#define ATZ_RPM_RING_BEZEL_D        360    // 细环直径（= 屏幕边）
#define ATZ_RPM_RING_BEZEL_W        10     // 细环线宽（参照物用 10）
#define ATZ_RPM_RING_BEZEL_COLOR    0      // 0 = 跟随主题文字色；或写死 0xRRGGBB
#define ATZ_RPM_RING_BEZEL_OPA      255    // 细环不透明度

// ── ② 转速弧（obd_brz_gauge 的 ui_RpmPageArcRpmBack）──────────────────────
#define ATZ_RPM_RING_D              340    // 弧直径（参照物用 340）
#define ATZ_RPM_RING_W              20     // 弧线宽（参照物用 20）
#define ATZ_RPM_RING_ROUNDED        0      // 参照物是**直角**（0），要圆头改 1
#define ATZ_RPM_RING_TRACK_COLOR    0      // 底槽色：0 = 主题文字色（再用下面的 OPA 压暗）
#define ATZ_RPM_RING_TRACK_OPA      70     // 底槽不透明度 0~255（实心底槽太抢眼，参照物在深色底上用了 #333）
#define ATZ_RPM_RING_BASE_COLOR     0      // 指示弧基色：0 = 跟随主题文字色；浅底建议 0x0A84FF、深底 0xFFFFFF

#define ATZ_RPM_RING_MAX_RPM        8000   // 环走满一圈对应的转速
#define ATZ_RPM_RING_WARN_RPM       5500   // 到这里的弧变琥珀色
#define ATZ_RPM_RING_ALARM_RPM      6500   // 到这里的弧变红色（与 car_alarm 阈值一致）
#define ATZ_RPM_RING_WARN_COLOR     0xFFA000
#define ATZ_RPM_RING_ALARM_COLOR    0xFF3B30

// ── ③ 节奏 ────────────────────────────────────────────────────────────────
#define ATZ_RPM_RING_DATA_MS        100    // 从车况缓存取新目标值的周期（ms）= 主表 10Hz
#define ATZ_RPM_RING_TICK_MS        6      // 插值步进（6ms ≈ 166fps 上限；实测 ~83fps）
#define ATZ_RPM_RING_STEP           20     // 死区（rpm）：目标变化小于这个值就不换目标
#define ATZ_RPM_RING_PUSH_STEP      4      // 显示值变化不到这个数就不写控件
// ★ 语音优先：设备**不在空闲状态**时（连接/聆听/说话），环的插值按这个分母降频。
//   跑 12ms 全速时 LVGL 约占 25% CPU + 大量总线带宽，对话时降载能明显减少音频卡顿。
//   设 1 = 不降频。
#define ATZ_RPM_RING_BUSY_DIV       3      // 非空闲时每 3 个 tick 才插值一次（≈28fps）
// LVGL 渲染任务优先级提升：上游给的是 1（最低），会被音频任务压住 → 动画卡。
// 这里在启动后把它抬到下面的值（0 = 不改）。仍低于音频任务，不影响语音链路。
#define ATZ_LVGL_TASK_PRIORITY      3
// ★ 整屏刷新（2026-09-18）：**实测不可行，已默认关闭**。
//   试过把 LVGL 缓冲换成"整屏 PSRAM + RENDER_MODE_FULL"，结果：
//     E spi_common: spicommon_dma_setup_priv_buffer(460): Failed to allocate priv TX buffer
//     E lcd_panel.io.spi: panel_io_spi_tx_color(406): spi transmit (queue) color failed
//     E event: create task for loop failed / WifiManager: Event loop create failed
//   根因：**内部 SRAM 只剩 ~13KB**（`SystemInfo: free sram: 12951`）。SPI 驱动要一块
//   内部 DMA bounce buffer 才能把 PSRAM 里的像素发出去，分配不到 → 整条刷屏链路失效，
//   连 WiFi 事件循环都建不起来（内存被挤干）→ 任务看门狗 11 秒后触发。
//   所以整屏"一次性"刷新在这块板子上做不到，只能继续用上游的 360×20 条带缓冲。
//   （保留开关是为了以后有人换了屏/腾出 SRAM 时可以再试。）
#define ATZ_FULL_FRAME_REFRESH      1

// LVGL 刷新定时器周期（ms）：Kconfig 默认 33ms。改成 16 后动画上限 ~60fps。0 = 不改。
#define ATZ_LVGL_REFR_PERIOD_MS     2
// 时钟（状态文字）纵向位置：在 ATZ_ROUND_STATUS_Y 基础上再往下挪这么多像素
#define ATZ_CLOCK_Y_NUDGE           5

// ═══════════════════════════════════════════════════════════════════════════
// 7. 字幕「逐字贴弧」（**主界面底部**）
//    上游的字幕是一条直线贴在圆屏底部 → 两端必被圆边切掉。
//    这里把整句拆成单字，逐字放在一段圆弧上并旋转到切线方向（笑脸形），
//    文字就顺着屏幕边缘走，同样半径下能放更多字。
//
//    几何：文字弧半径 ATZ_ARC_TEXT_RADIUS，以屏幕中下方的 ATZ_ARC_TEXT_ANGLE 为中心
//    （0° = 正下方，向两侧展开 ±ATZ_ARC_TEXT_SPAN）。
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_ARC_TEXT_ENABLE         0      // ★ 已关：实测会让语音对话变卡、且长句被截断，用户要求回退上游直线字幕
#define ATZ_ARC_TEXT_HIDE_BAR       1      // 1 = 连上游那条矩形字幕栏一起藏掉，只留弧形文字
#define ATZ_ARC_TEXT_RADIUS         138    // 文字基线所在半径（px）—— 必须在转速环内侧
#define ATZ_ARC_TEXT_ANGLE          0      // 弧的中心方向：0 = 正下方，负 = 偏左
#define ATZ_ARC_TEXT_SPAN           62     // 单侧最大展开角度（度）
#define ATZ_ARC_TEXT_MAX_CHARS      20     // 最多几个字（超了截断加省略号）
#define ATZ_ARC_TEXT_COLOR          0xFFFFFF  // 0 = 跟随主题文字色
#define ATZ_ARC_TEXT_POLL_MS        200    // 轮询上游字幕文本的周期（变了才重排）

// ═══════════════════════════════════════════════════════════════════════════
// 9. 省电与观感（2026-09-16 优化）
//
// 9.1 字幕冻结：上游字幕是 LONG_SCROLL_CIRCULAR —— **文本比框宽就永远滚**。
//     实测：光这一项就让设备在空闲时也跑 28fps（≈14% CPU 常年白烧）。
//     现在：新字幕先滚 N 毫秒（够看清），之后切成长度裁剪模式停住；来了新字幕再解冻。
#define ATZ_SUBTITLE_FREEZE         1      // 0 = 保持上游行为（一直滚）
#define ATZ_SUBTITLE_ROLL_MS        8000   // 每条字幕允许滚动多久（ms）
#define ATZ_SUBTITLE_POLL_MS        500    // 检查字幕是否变化的周期（ms）
// 字幕静置多久后**自动清空**（0 = 不清空）。
// ★ 2026-09-17 用户报"主界面下面一直显示 rpm 什么" —— 根因是车况告警的正文
//   （"7100 rpm (limit 6500 rpm)"）是走 SetChatMessage 显示在字幕上的，而告警在超限
//   期间会重发同一条文本，"文本没变就保持冻结"让它永远停在屏幕上。
//   现在静置 30 秒就清掉（说话/聆听中不清，避免抹掉正在播报的字幕）。
#define ATZ_SUBTITLE_HOLD_S         30

// 9.2 动态帧率：转速变化慢时没必要 166Hz 地插值。
//     实测扫掠（转速飞变）时 83fps 占 ~66% CPU；缓慢漂移时降到 1/3 帧率完全看不出来。
#define ATZ_RPM_RING_SLOW_DELTA     150    // 目标与显示值相差小于这个（rpm）就算"慢"
#define ATZ_RPM_RING_SLOW_DIV       3      // 慢的时候每 N 个 tick 才插值一次

// ═══════════════════════════════════════════════════════════════════════════
// 10. 视觉告警（车上噪声大，声音告警常常听不见 → 屏幕也要给信号）
//     实现：轮询 car_alarm_active()，有活动告警时让**外沿细环闪红**（3Hz），
//     告警解除后再多闪一段时间（余辉），避免一闪而过没注意。
//     ★ 2026-09-16：转速环被用户删掉了，所以这套"闪细环"的可视告警**暂时没有载体**
//       （配置与代码留着，环一开就恢复）。当前告警只有：蜂鸣 + 车况页把超限数值标红。
//       以后想做独立载体，最省事的是让车况页的链路圆点/顶部提示闪红。
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_ALERT_FLASH_ENABLE      1      // 0 = 关闭视觉告警
#define ATZ_ALERT_FLASH_MS          15000  // 单次告警最多闪这么久（ms）
#define ATZ_ALERT_GRACE_MS          3000   // 告警解除后补闪（ms）
#define ATZ_ALERT_POLL_MS           250    // 轮询告警状态的周期（ms）
#define ATZ_ALERT_FLASH_HZ          3      // 闪烁频率（Hz）
#define ATZ_ALERT_COLOR             0xFF3B30   // 闪烁色（与告警红一致）

// ═══════════════════════════════════════════════════════════════════════════
// 11. 自动调光（夜间/长时间无交互时降亮）——**已关闭**
//     2026-09-16 用户反馈："自动亮度也是不需要、因为亮度总是在变很不舒服"。
//     所以 ATZ_DIM_ENABLE 置 0：背光只在启动时设一次，之后**永不被本模块改动**。
//     实现与参数都还留着（改回 1 就恢复），省得以后想用时重写。
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_DIM_ENABLE              0      // 0 = 不自动调光（用户要求：亮度别自己变）
#define ATZ_DIM_AFTER_S             120    // 连续这么久没有交互（且不在对话）就降亮
#define ATZ_DIM_PCT                 20     // 降到的亮度百分比
#define ATZ_DIM_POLL_MS             1000   // 检查周期（ms）
// 夜间再暗一档（需要 NTP 时间；不联网就退化成只用上面的空闲降亮）
#define ATZ_DIM_NIGHT_ENABLE        1
#define ATZ_DIM_NIGHT_FROM_HOUR     22     // 22:00 起
#define ATZ_DIM_NIGHT_TO_HOUR       6      // 到 06:00
#define ATZ_DIM_NIGHT_PCT           10     // 夜间的降亮目标

// ═══════════════════════════════════════════════════════════════════════════
// 13. 换挡提示灯：**已删除**（2026-09-16 用户要求）
//     "换挡提示灯删除掉吧、转速环也删除掉" —— 车上本来就有换挡灯，屏幕再闪一个
//     既分心，又要和"告警红"抢同一条细环。相关代码（细环闪绿、NVS 键 shift_rpm、
//     语音工具 self.ui.set_shift_light、/ring?shift=）全部移除，要恢复看 git 历史。
// ═══════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════
// 14. 行程统计与峰值保持（**独立「行程」页 + 语音开始/重置记录**）
//     · 记录默认**关**：用户说「开始记录行程」才累加；说「暂停/结束记录」就停。
//     · 峰值：最高转速/车速/水温/油温/进气/负荷；最低电压
//     · 里程：对车速积分得到（没有里程表，这是估算值，统一标"约"）
//     · 超转时长：转速 ≥ ATZ_TRIP_HOT_RPM 的累计秒数（换挡灯删了，改用固定值）
//     寿命考虑：全程在 RAM 里累加，**每 60 秒才写一次 NVS**（断电最多丢 1 分钟）。
//     页面：车况页按一下（点屏/K 端点）翻到第二页；语音「看行程统计」也能直达。
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_TRIP_ENABLE             1
#define ATZ_TRIP_POLL_MS            200    // 采样周期（ms）；峰值要抓得住尖峰，别太慢
#define ATZ_TRIP_SAVE_S             60     // 每这么久把统计写进 NVS（0 = 不持久化）
#define ATZ_TRIP_HOT_RPM            6500   // 「高转时长」的判定门槛（rpm）
#define ATZ_TRIP_RECORD_DEFAULT     0      // 开机是否自动开始记录（0 = 手动开始，默认）
// ═══════════════════════════════════════════════════════════════════════════
// 12. 调试端点鉴权（防同网段随手乱改，不是强安全措施）
//     写操作（/theme /carbar /carpage /size /inject /ring /subtitle）要求带
//       &key=<ATZ_DEBUG_TOKEN>
//     只读端点（/health /shot.jpg /layout /perf /touch）不受影响。
//     token 会打印在串口启动日志里；PC 端模拟台会自动从本文件读它。
//     想彻底关掉调试服务：把 ATZ_UI_SHOT_SERVER_ENABLE 置 0。
//
//     ★ 私密值不写在仓库里：本目录下的 atz_local.h 不进 git（.gitignore 已排除），
//       在它里面重新定义 ATZ_DEBUG_TOKEN（写你自己的口令）就会覆盖下面的占位值
//       —— 覆盖机制见本文件最开头的 0.0.1 节。
//       只用占位值时写接口依然带鉴权，只是口令是公开的 —— 家里局域网够用；
//       想更严就建 atz_local.h（模板见 开发参考/atz_local.h.example）。
#define ATZ_DEBUG_AUTH              1      // 1 = 写操作校验 token
#ifndef ATZ_DEBUG_TOKEN
#define ATZ_DEBUG_TOKEN             "atz-local-debug"   // 公开占位值；真口令放 atz_local.h
#endif
// ═══════════════════════════════════════════════════════════════════════════
// 8. 台架注入的硬上限
//    教训（2026-09-16）：模拟一旦启动就一直在注入假数据，设备看起来"停不下来"。
//    现在有两条路停车：① 到点自动停 ② `/carbar?mode=stop` 或语音 mode="stop" 立即停。
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_SIM_MAX_SECONDS         120    // 单次模拟最长秒数（上限，防止忘了停）
