# AtzGauge · 马自达阿特兹 2020 车载智能仪表

把小智 AI（[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)）改造成一块**圆形车载仪表**：
插 OBD 口读车况，ESP-NOW 无线送到方向盘旁的圆屏上，同时保留语音对话能力。

- **硬件**：微雪 ESP32-S3-Touch-LCD-1.85（1.85" 圆屏 360×360，SKU 28514）+ Vgate iCar Pro 2S（BLE ELM327）
- **车**：马自达阿特兹 2020 运动版
- **状态**：台架（bench）全链路已验证，**上车首测待做** —— 见 [`开发参考/15-上车首测清单.md`](开发参考/15-上车首测清单.md)

---

## 这套东西长什么样

| 页面 | 内容 |
|---|---|
| **主界面（表情页）** | 表情动画 + 顶部时钟 + 车况常显条（转速/车速/水温…），点屏说话，**左划**进车况页 |
| **车况页（第 1 页）** | ESP-NOW 主表 10Hz 的 10 个字段整屏显示（转速/车速/水温/油温/进气/负荷/节气门/电压/油压/空燃比），2 秒无数据转灰，**右划**回主界面 |
| **行程统计页（第 2 页）** | 里程 / 最高转速 / 平均与最高车速 / 高转时长 / 行驶时长，可手动开始记录、一键清零 |

语音能直接控制界面：「换成夜间模式」「把车况条关掉」「看行程统计」「模拟一下转速」等
（注册为 MCP 工具 `self.ui.*` / `self.car.*`，走小智云端）。

## 架构

```
                 BLE                     ESP-NOW                SPI/QSPI
 车 ── OBD2 ── Vgate iCar Pro 2S ──► 主表 ESP32 ──► (2.4GHz) ──► 从表 = 本仪表 ──► 圆屏
                                          │                          │
                                     读 PID、算字段              显示 + 语音（小智 AI 云端）
```

- **主表**：另一块 ESP32，负责 OBD 解析与 ESP-NOW 广播（本项目只含从表固件；主表固件在 `obd_brz_gauge` 分支/仓库）
- **从表**：本仓库的板型 `waveshare/esp32-s3-touch-lcd-1.85-atzgauge`，在官方小智固件基础上加 UI 与车况链路
- ⚠️ **路由器/热点必须在 2.4GHz 信道 1**（ESP-NOW 与 WiFi 共用信道，主表固定在信道 1）

## 编译与烧录

```powershell
# 1) 准备官方固件源码（本仓库不含上游，1.19 GB 太大）
git clone --depth 1 https://github.com/78/xiaozhi-esp32.git xiaozhi-esp32/src

# 2) 打上本项目的改动（补丁 + 新增文件，幂等可重跑）
cd xiaozhi-esp32/mods-atzgauge
node apply-mods.mjs          # 打补丁
node verify-mods.mjs         # 校验：39 项就位

# 3) 编译（ESP-IDF v6.1；本机用 tools/idf-run.ps1 包了一层环境激活）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf-run.ps1 `
    -Command "python scripts/build.py waveshare/esp32-s3-touch-lcd-1.85-atzgauge"

# 4) 烧录（把 COM3 换成你的串口）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\idf-run.ps1 `
    -Command "cmd /c tools\flash-atzgauge.bat"
```

> 上游文件**只改了 2 个**（`main/CMakeLists.txt`、`main/Kconfig.projbuild`），其余全是新增文件。
> 升级上游后跑一次 `apply-mods.mjs` 即可，细节见 [`mods-atzgauge/MODIFICATIONS.md`](xiaozhi-esp32/mods-atzgauge/MODIFICATIONS.md)。

## 界面调参：只改一个文件

[`atz_ui_config.h`](xiaozhi-esp32/src/main/boards/waveshare/esp32-s3-touch-lcd-1.85-atzgauge/atz_ui_config.h)
是**唯一调参面板**：主题、车况条、字号、时钟位置、行程统计、刷屏方式、告警、调光……全在里面，改完重编译即可，不用碰上游代码。

**调试口令等私密值不放仓库**：板目录下的 `atz_local.h` 不进 git，在那里重新 `#define` 即可覆盖默认值
（模板见 [`开发参考/atz_local.h.example`](开发参考/atz_local.h.example)）。没有这个文件也能正常编译。

## 仓库结构

| 路径 | 内容 |
|---|---|
| `xiaozhi-esp32/src/main/boards/waveshare/esp32-s3-touch-lcd-1.85-atzgauge/` | 板型：UI、车况页、行程统计、调试端点（`atz_ui_config.h` 是调参入口） |
| `xiaozhi-esp32/src/main/espnow_slave/` | ESP-NOW 从表、车况数据缓存、告警规则 |
| `xiaozhi-esp32/mods-atzgauge/` | 升级维护套件：patch + 新增文件快照 + 一键重打/校验脚本 |
| `开发参考/` | 全部开发文档：路径规范、环境配置、迭代日志与缺陷台账、界面清单、几何实测、表情包规范 |
| `tools/` | 构建/烧录/抓日志/尺寸量测/表情包推送/模拟台/仓库备份等脚本（多数可双击 `.cmd` 运行） |

**想快速了解全貌** → [`开发参考/00-总览与索引.md`](开发参考/00-总览与索引.md)
**动手改代码前** → [`开发参考/18-迭代日志-结构变更与缺陷台账.md`](开发参考/18-迭代日志-结构变更与缺陷台账.md)（含所有踩过的坑）
**改界面** → [`开发参考/20-界面元素清单与页面清单.md`](开发参考/20-界面元素清单与页面清单.md) 与 [`21-界面几何总表（实测）.md`](开发参考/21-界面几何总表（实测）.md)

## 常用工具（`tools\`）

| 工具 | 用途 |
|---|---|
| `idf-run.ps1` | 在 ESP-IDF v6.1 环境里跑任意命令（本机执行策略不允许 dot-source） |
| `flash-atzgauge.bat` / `boot-log.ps1` | 烧录 / 抓串口启动日志 |
| `emoji-kit.cmd` | **换表情包**：体检→打包→本地服务器→设备下载，不用重刷固件（双击可用） |
| `sim-console.cmd` | **车况模拟台**：PC 端 10Hz 发模拟转速等数据给设备，台架验证用（双击可用） |
| `backup-repo.cmd` | 把仓库备份到另一块物理盘（默认 `E:\backup\AtzGauge.git`） |

设备 IP 用 `--device <IP>` 或环境变量 `ATZ_DEVICE` 指定；调试口令用 `--key=` 或 `ATZ_DEBUG_TOKEN`。

## 许可与出处

- 基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（MIT）修改；本仓库**只含我们自己写的部分**，不含上游源码
- 上游协议与文档归档在 [`开发参考/xiaozhi-ai/`](开发参考/xiaozhi-ai/)（版权归原作者）
- OBD 解析思路参考 [steveEcode/obd_brz_gauge](https://github.com/steveEcode/obd_brz_gauge)
- 本仓库自有代码沿用 MIT，见 [`LICENSE`](LICENSE)

> ⚠️ **安全提示**：车上接线涉及 12V 与安全气囊相关线束，动手前务必读
> [`马自达仪表-制作指南.md`](马自达仪表-制作指南.md) 的「安全红线」与「常见错误」两节。
