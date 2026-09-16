# AtzGauge 对 xiaozhi-esp32 的改动台账

> **这份文件解决一个问题**：官方小智固件更新后，如何**快速**把我们的改动重新打上去。
>
> 基线：`78/xiaozhi-esp32` **v2.5.0**（`codeload.github.com` 拉的原版 tar.gz）
> 校验方式：把套件应用到一份纯净 v2.5.0 上，得到的树与当前工作树**逐字节一致**（已实测）。

---

## 一、总览：改动只有两类

| 类别 | 数量 | 升级时的影响 |
|---|---|---|
| **改动上游文件** | **2 个**（3 处，全部集中在连续块里） | 需要重打 → `apply-mods.mjs` 自动做 |
| **纯新增文件** | **15 个** | 整份拷贝，永远不冲突 → `apply-mods.mjs` 自动做 |
| 生成物 | `build/`、`managed_components/`、`sdkconfig`、`main/assets/lang_config.h`、`scripts/__pycache__/` | 不要动、也不进 patch |

### 上游文件里改了哪 3 处

| # | 文件 | 锚点（找不到就按这个搜） | 内容 |
|---|---|---|---|
| **A1** | `main/CMakeLists.txt` | `elseif(CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_1_85)` **之前** | 板型选择分支：设 `BOARD_DIR`、字号档位（20px）、表情集（128px） |
| **A2** | `main/CMakeLists.txt` | `idf_component_register(SRCS ${SOURCES}` **之前** | `list(APPEND SOURCES ...)` 加 4 个 ESP-NOW 源文件；`INCLUDE_DIRS` 加 `espnow_slave`；`MAIN_PRIV_REQUIRES_EXTRA` 加 `esp_wifi` |
| **A3** | `main/Kconfig.projbuild` | `config BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_3_49` **之前** | 新增板型 Kconfig 条目 `BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_1_85_ATZGAUGE` |

三处都包着醒目的 `══════` 分隔线与中文说明，在 diff 里一眼可辨。

> **为什么改动这么少**：AI 的 `AGENTS.md` 要求"板级行为不要写进核心模块"，而
> `main/CMakeLists.txt` 用 `file(GLOB BOARD_SOURCES ...)` 自动收集板型目录下的所有 `.cc`。
> 所以**我们绝大部分代码都放在板型目录里，一个字节的上游代码都不用碰**。

---

## 二、新增文件清单（15 个，全部是我们自己的）

### 2.1 板型目录 `main/boards/waveshare/esp32-s3-touch-lcd-1.85-atzgauge/`（7 个）

| 文件 | 职责 |
|---|---|
| `config.json` | 板型标识 `esp32-s3-touch-lcd-1.85-atzgauge`。**★ 这个标识决定 OTA 通道**，必须与官方 1.85 不同，否则会被官方固件静默覆盖 |
| `config.h` | 引脚表 / 屏幕参数（与官方 1.85 逐字节一致，只多出 `TP_*` 触摸定义） |
| `esp32-s3-touch-lcd-1.85-atzgauge.cc` | 板级装配：ST77916 屏幕、TCA9554、按键、**触摸唤醒**、ESP-NOW 从表启动、车况告警、MCP 工具注册 |
| `atz_ui_config.h` | **★ UI 唯一调参面板**：3 套主题的全部颜色、车况条宽度/位置/字段/刷新率、运行时持久化键 |
| `atz_ui.h` / `atz_ui.cc` | 主题包注册、车况条构建与刷新、`self.ui.set_theme` / `self.ui.set_car_bar` 两个语音工具 |
| `README.md` | 板型说明：编译、烧录、行为、排查表 |

### 2.2 ESP-NOW 从表 `main/espnow_slave/`（8 个）

| 文件 | 职责 |
|---|---|
| `espnow_link.c` / `.h` | ESP-NOW 接收。**刻意不初始化 WiFi**：信道跟随 STA，因此硬约束是"路由器必须在信道 1" |
| `obd_data_cache.c` / `.h` | 车况缓存（自主表项目 `obd_brz_gauge` 裁剪：去掉档位/里程/RS485） |
| `car_alarm.cc` / `.h` | 阈值告警（水温/油温/转速/电压）+ 本地预录音频 + 陈旧数据门控 |
| `car_status_tool.cc` / `.h` | 语音问答工具 `self.car.get_status`，含链路诊断话术 |

---

## 三、升级官方固件的标准流程

```powershell
# 0) 备份当前可用的树（出问题能退回）
#    例：复制 D:\AtzGauge\xiaozhi-esp32\src 到 backup\xiaozhi-src-<日期>\

# 1) 下载新版官方源码，解压成新树（不要直接覆盖旧树）
#    https://github.com/78/xiaozhi-esp32  →  codeload tar.gz

# 2) 把我们的改动打上去（幂等：已就位的会跳过；--dry-run 可先看会做什么）
node D:\AtzGauge\xiaozhi-esp32\mods-atzgauge\apply-mods.mjs D:\path\to\new\xiaozhi\src

# 3) 校验（apply 之后会自动跑一遍；也可以单独跑）
node D:\AtzGauge\xiaozhi-esp32\mods-atzgauge\verify-mods.mjs D:\path\to\new\xiaozhi\src

# 4) 编译 + 烧录
powershell -NoProfile -ExecutionPolicy Bypass -File D:\AtzGauge\tools\idf-run.ps1 `
    -Command "python scripts/build.py waveshare/esp32-s3-touch-lcd-1.85-atzgauge"
powershell -NoProfile -ExecutionPolicy Bypass -File D:\AtzGauge\tools\idf-run.ps1 `
    -Command "cmd /c D:\AtzGauge\tools\flash-atzgauge.bat"
```

**如果第 2 步报"插不进去"**：说明上游改动动了那一带的写法。脚本会把锚点和块原文告诉你，
手工插一下即可 —— 块原文都在 `blocks/` 里，三块分别只有 19 / 20 / 9 行。

### 套件里都有什么

| 文件 | 用途 |
|---|---|
| `MODIFICATIONS.md` | 本文件（人读台账） |
| `manifest.json` | 机器可读清单：新增文件表、锚点、抽查项 |
| `modified-upstream.patch` | 上游 2 个文件的真实 diff（`git apply -p1` 可用，3 个 hunk） |
| `new-files/` | 15 个新增文件的整份快照 |
| `blocks/` | 3 处上游改动的**原文**（`apply-mods.mjs` 就是把它插进去） |
| `apply-mods.mjs` | 一键重打（幂等 + 自动校验） |
| `verify-mods.mjs` | 只读校验，退出码 0/1 可进 CI |

### 改动套件本身怎么更新

改了源码之后，重新生成一次即可（会重新算 diff、刷新 new-files 与 blocks）：

```powershell
node D:\AtzGauge\tools\gen-mods-kit.mjs
```

需要一个纯净上游副本作为基线，默认路径 `D:\AtzGauge\upstream\xiaozhi-esp32-2.5.0`
（拉取：`https://codeload.github.com/78/xiaozhi-esp32/tar.gz/refs/tags/v2.5.0`）。

---

## 四、升级后要特别留意的地方

| 事项 | 说明 |
|---|---|
| **板型标识不能变** | `config.json` 的 `type` 决定 OTA 通道。改了会让设备被官方固件覆盖，或被判成"另一台设备" |
| **NVS 键是持久化 API** | 我们用了 `display/theme`（与上游共用）和 `atz_ui/car_bar`、`atz_ui/ui_migrated`。改键名必须写迁移 |
| **上游 UI 结构变了怎么办** | 我们的车况条是"叠加式"（先让上游 `SetupUI()` 建好，再在 `screen` 上加一层），只依赖 `lv_screen_active()` 和主题对象。若上游改了主界面层级，最多是位置需要微调，不会崩 |
| **`esp_lcd_panel_io` 的坑** | 触摸读寄存器不要走 `esp_lcd_panel_io`：它把每次 NACK 都打成 ERROR。用原生 `i2c_master`（详见 `开发参考/17` 第十节） |
| **字号档位** | 20px 是我们改的；若要回 16px，改 A1 块里那两行（`font_noto_sans_basic_20_4` → `_16_4`） |
| **表情集** | 128px（官方 1.85 是 64px）。代价：资产分区约 2.4MB / 8MB |

---

## 五、变更历史

| 日期 | 变更 | 涉及 |
|---|---|---|
| 2026-09-14 | 首次建立套件。此时相对 v2.5.0 的改动 = 从表移植 + 独立板型 + 128px 表情 + 触摸唤醒 + UI 定制 | 2 改 / 15 增 |
| 2026-09-14 | 把散落在 `set(SOURCES ...)`、`set(INCLUDE_DIRS ...)`、`PRIV_REQUIRES` 里的改动**收敛成 2 个连续块**，减少升级时的重打点 | CMakeLists 由 3 处散布 → 2 个块 |
| 2026-09-14 | 修掉板级 `InitializeTca9554()` 里 6 行双重编码乱码注释；恢复被注释吞掉的 `ESP_ERROR_CHECK(ret)` | 板型 `.cc` |
