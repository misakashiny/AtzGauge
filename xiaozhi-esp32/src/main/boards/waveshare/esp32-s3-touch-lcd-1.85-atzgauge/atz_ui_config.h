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
// 0.0 固件标识（★ 每次启动打一条，/health 也回它）
//     为什么要有：台架上"现在设备里跑的到底是哪一版"以前只能靠比对日志猜。
//     现在串口启动日志和 http://<IP>:8099/health 都能直接看到构建时间与关键开关。
//     改完重要功能顺手把 ATZ_FW_STAGE 往前推一格（人读用）。
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_FW_NAME                 "atzgauge"
#define ATZ_FW_STAGE                "stage12-ring-flush-73fps"   // 版本阶段名（**保持 ASCII**：/health 是机器读的，中文经 HTTP 易乱码）
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
#define ATZ_ROUND_SUBTITLE_WIDTH     160   // 字幕文字宽度上限：要躲开加厚后的转速环内缘（半径 154）
#define ATZ_ROUND_SUBTITLE_Y_OFFSET  36    // 字幕栏整体上移：底部那一圈现在是转速环，字幕要待在环内侧

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
// 6. 转速外圈（**不受 ATZ_UI_ENABLE 影响，始终生效**）
//    ★ 只画**一个环**：整圈 = 底槽，跟着转速点亮的一段 = 指示弧（参考 obd_brz_gauge）。
//
//    ★★ 关于"完全贴死、不留缝隙"（2026-09-16 二改，实测记录）★★
//      · LVGL 的弧是"从对象半径**往内**画"的：
//          d=340 / w=20 → 只占半径 150~170（离屏幕边 10px，早期版本的"离边"就是这里来的）
//          d=360 / w=26 → 占半径 154~180（外缘正好到面板边）
//      · 像素扫描确认：外缘确实到了 x=0 / y=0 / y=359（帧缓冲最外圈），**已经没有像素可让**。
//      · 那"看着还有几个像素"是什么？—— **底槽太淡**（不透明度 40/255，浅色主题下
//        几乎看不见），眼睛只把暗色那段当"环"，于是把外面淡掉的一圈当成了空隙。
//      · 所以最终做法是两条一起：
//          ① 直径放大到 **380**（比屏幕还大）→ 弧的外半圈被面板边缘**直接裁掉**，
//             不管怎么量都不可能再出现缝隙（这也是唯一能"贴死"的可靠办法：
//             软件最多画到面板最后一像素，那就干脆画出去、让面板去裁）；
//          ② 底槽不透明度提到 **90** → 整圈都是清清楚楚的环，不会再看错。
//      · 若仍觉得与外壳之间有空隙，那圈空隙在**面板可寻址像素之外**（模组的黑边/玻璃边），
//        软件无法绘制 —— 判断方法：让整屏显示纯白（浅色主题），看白色是否一直铺到玻璃边。
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_RPM_RING_ENABLE         1      // 总开关
#define ATZ_RPM_RING_D              380    // 环对象直径（px）：>360 = 故意让外缘被屏幕裁掉
#define ATZ_RPM_RING_W              36     // 环线宽（px）：可见部分约 154~180（26px）
#define ATZ_RPM_RING_TRACK_OPA      90     // 底槽不透明度 0~255（整圈要看得见）
#define ATZ_RPM_RING_MAX_RPM        8000   // 环走满一圈对应的转速
#define ATZ_RPM_RING_WARN_RPM       5500   // 到这里的弧变琥珀色
#define ATZ_RPM_RING_ALARM_RPM      6500   // 到这里的弧变红色（与 car_alarm 阈值一致）

// ── 外观（2026-09-16 美化）──────────────────────────────────────────────────
#define ATZ_RPM_RING_ROUNDED        1      // 弧线两端**圆头**：更像真表，观感柔和很多
#define ATZ_RPM_RING_BASE_COLOR     0      // 指示弧基色；0 = 跟随主题文字色（深色主题=白、浅色主题=近黑）
                                           //   想固定就写 0xRRGGBB：浅底建议 0x0A84FF（蓝），深底建议 0xFFFFFF
#define ATZ_RPM_RING_WARN_COLOR     0xFFA000   // 接近上限：琥珀
#define ATZ_RPM_RING_ALARM_COLOR    0xFF3B30   // 超限：红（与告警/车况页同色）
// 红区刻度：在环**内侧**再画一条细弧标出"红线区"（ALARM_RPM → 满圈）。
// 为什么放内侧：指示弧占半径 154~190，标在 148~152 不会被它盖住；而且它是**静态**的
// （只在首帧与"指示弧恰好扫过它"时重绘），几乎不占帧预算。
#define ATZ_RPM_RING_REDZONE        1      // 0 = 不画红区刻度
#define ATZ_RPM_RING_REDZONE_W      4      // 红区刻度线宽（px）
#define ATZ_RPM_RING_REDZONE_GAP    3      // 与环内缘的间距（px）
#define ATZ_RPM_RING_REDZONE_OPA    200    // 红区刻度不透明度 0~255
// 取数节奏：主表就是 10Hz
#define ATZ_RPM_RING_DATA_MS        100    // 从车况缓存取新目标值的周期（ms）
#define ATZ_RPM_RING_STEP           20     // 死区（rpm）：目标变化小于这个值就不换目标
// ★ 流畅度：**不用 lv_anim**。LVGL 的动画定时器周期由编译期的 LV_DEF_REFR_PERIOD 决定
//   （本项目 = 33ms），动画最多 ~30fps 就到顶了 —— 实测 27~28fps 正是撞在这个天花板上。
//   改成我们自己按 16ms 步进做插值（每步走剩余差的 1/3），就能跑到 ~60fps。
#define ATZ_RPM_RING_TICK_MS        6      // 插值步进周期（6ms ≈ 166fps 上限；实测 ~90fps，见 21 号文档 5.5）
#define ATZ_RPM_RING_PUSH_STEP      4      // 显示值变化不到这个数就不写控件（4rpm ≈ 0.18°，看不出来）
// ★ 语音优先：设备**不在空闲状态**时（连接/聆听/说话），环的插值按这个分母降频。
//   跑 12ms 全速时 LVGL 约占 25% CPU + 大量总线带宽，对话时降载能明显减少音频卡顿。
//   设 1 = 不降频。
#define ATZ_RPM_RING_BUSY_DIV       3      // 非空闲时每 3 个 tick 才插值一次（≈28fps）
// LVGL 渲染任务优先级提升：上游给的是 1（最低），会被音频任务压住 → 动画卡。
// 这里在启动后把它抬到下面的值（0 = 不改）。仍低于音频任务，不影响语音链路。
#define ATZ_LVGL_TASK_PRIORITY      3
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
// 8. 台架注入的硬上限
//    教训（2026-09-16）：模拟一旦启动就一直在注入假数据，设备看起来"停不下来"。
//    现在有两条路停车：① 到点自动停 ② `/carbar?mode=stop` 或语音 mode="stop" 立即停。
// ═══════════════════════════════════════════════════════════════════════════
#define ATZ_SIM_MAX_SECONDS         120    // 单次模拟最长秒数（上限，防止忘了停）
