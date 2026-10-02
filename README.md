# AtzGauge · 马自达阿特兹 2020 车载智能仪表

把小智 AI（[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)）改造成一块**圆形车载仪表**：
插 OBD 口读车况，ESP-NOW 无线送到方向盘旁的圆屏上，同时保留语音对话能力。

- **从表（= 本仪表）**：微雪 ESP32-S3-Touch-LCD-1.85（1.85" 圆屏 360×360，SKU 28514）+ Vgate iCar Pro 2S（BLE ELM327）
- **主表（= 车上的数据源）**：同型号板，跑上游 `obd_brz_gauge` 固件
- **车**：马自达阿特兹 2020 运动版（协议 6，ISO 15765-4 CAN@500k）
- **状态**：台架全链路已验证；**上车首测未完成**

---

## 🚀 接手第一件事

```
1. 读 docs/00_交接总纲.md          ← 环境事实与架构约束（不照做必失败）
2. 读 docs/01_迭代清单.md          ← 还剩什么没做 + 已排除的非目标
3. 跑一次从表编译                   ← 确认工具链可用（不要跳过）
4. 读 开发参考/15-上车首测清单.md    ← 当前主线任务
```

要改代码时直接查 **`docs/02_源码导读.md`** 的「改 X 动哪里」速查表。

---

## 📁 目录地图

| 路径 | 体积 | 说明 |
|---|---|---|
| **`docs/`** | 28 K | ★**交接文档**：`00_交接总纲` / `01_迭代清单` / `02_源码导读` + `patches/` |
| **`开发参考/`** | 604 K | ★**项目全部记忆**：25 份台账文档，入口是 `00-总览与索引.md` |
| **`tools/`** | 261 K | ★构建 / 烧录 / 抓日志 / 模拟台 / 表情包 / 备份等 42 个脚本 |
| **`obd_brz_gauge/`** | 164 M | 主表工程。`repo/` 是**可构建的上游克隆**（含我们的帧率补丁） |
| **`xiaozhi-esp32/`** | 50 M | 从表工程。`src/` 上游源码，`mods-atzgauge/` 升级套件 |
| **`backup/`** | 57 M | ★证据。**含出厂固件整片备份，不可再生** |
| `upstream/` | 13 M | 上游 v2.5.0 原始快照（校验 mods 套件用） |
| `android_app/` | 4.3 M | 配套安卓 App |
| `mytheme/` · `mazda-gauge/` | 68 K | 主题骨架 / 素材 |
| `马自达仪表-制作指南.md` | 12 K | ⚠️ **含「安全红线」与「常见错误」，动车上接线前必读** |
| **`_archive/`** | 859 M | **归档区，可整目录删除**（详见下） |

### `_archive/` 说明

装的是两个 ESP-IDF 工程的依赖目录（`managed_components`），本次整理时从工程里移出：

- **有网编译** → 不用管，IDF 会按 `dependencies.lock` 自动重下
- **想省 860 MB 下载** → 把目录移回原位（`xiaozhi-esp32/src/` 与 `obd_brz_gauge/repo/`）再编译
- **磁盘紧张** → **整个 `_archive/` 直接删掉**，零风险

---

## 🔧 快速命令

```powershell
# ── 从表（ESP-IDF v6.1 @ D:\esp）────────────────────────────
# ⚠️ 走 Python 启动器：本机执行策略是 Restricted，idf-run.ps1 里的
#    dot-source activate-idf.ps1 会被拒绝，脚本却不报错继续跑 →
#    cmake 找不到 ninja/git，报「unable to find a build program」
python tools/idf61-run.py --cmd "python scripts/build.py waveshare/esp32-s3-touch-lcd-1.85-atzgauge"

# 烧录 / 抓日志仍可用原脚本（它们不需要 IDF 环境）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\boot-log.ps1 -Seconds 70
```

```bash
# ── 主表（ESP-IDF v5.5.3 @ D:\esp553）──────────────────────
# ⚠️ 同样必须走 Python 启动器；直接调 idf.py 会静默 exit 0
python tools/idf553-run.py build
python tools/idf553-run.py -p COM3 app-flash
```

> ⚠️ **两个启动器不可混用**：`idf61-run.py` 用 `D:\esp`（v6.1，取**较新**工具链），
> `idf553-run.py` 用 `D:\esp553`（v5.5.3，取**较旧**工具链）。共用一个 `IDF_TOOLS_PATH`
> 时，`cmake` 有 3.30.2 / 4.0.3 两套，选错版本会直接构建失败。

> ⚠️ **串口唯一来源**：脚本参数 → `ATZ_PORT` → **`PORT.txt`** → `COM3`。换电脑只改 `PORT.txt`。
> ⚠️ **串口是独占的**：抓日志与烧录不能并行。

---

## ⚠️ 三条最容易踩的坑

| 坑 | 后果 | 对策 |
|---|---|---|
| **路径含中文/空格** | ESP-IDF 直接失效 | 只在 **`D:\AtzGauge`** 编译 |
| **直接调 `idf.py`（主表）** | 静默 exit 0，白等 | 走 `tools/idf553-run.py` |
| **用错 IDF 版本** | 编译失败 | 从表 v6.1 / 主表 v5.5.3，**不可互换** |

> 完整清单（7 条铁律）见 [`docs/00_交接总纲.md`](docs/00_交接总纲.md) 第三节。

---

## 🔐 不可再生资产

| 资产 | 位置 |
|---|---|
| **出厂固件整片备份**（16 MB） | `backup/archive/2026-09/firmware/factory-16MB.bin` |
| **改小智前的仪表固件**（16 MB） | `backup/archive/2026-09/firmware/obd-gauge-current-before-xiaozhi-16MB.bin` |
| 历次启动日志与界面实测截图 | `backup/`（133 项） |

> **`backup/` 不是垃圾目录**，整理时已刻意保留。`tools/prune-backup.mjs` 可瘦身（只移动不删除，默认空跑）。

---

## 📌 项目约束（速记）

- 两个 ESP-IDF 工程**并列，不能嵌套**；各自有 `CMakeLists.txt` / `sdkconfig` / `build/`
- 从表相对上游**只改 2 个文件（3 处连续块）**，其余全是新增文件；升级上游后跑 `node mods-atzgauge/apply-mods.mjs` 即可
- **板型标识 `esp32-s3-touch-lcd-1.85-atzgauge` 决定 OTA 通道，绝不能改**（改了会被官方固件静默覆盖）
- **路由器/热点必须锁信道 1**（主表把 ESP-NOW 信道硬编码为 1）
- 私密口令放板目录 `atz_local.h`（**不进 git**；模板见 `开发参考/atz_local.h.example`）
- 主表补丁是**未提交**的工作树修改，已导出到 `docs/patches/00-master-framerate.patch`

---

## 版本管理

| 远端 | 地址 | 用途 |
|---|---|---|
| `origin` | `E:\backup\AtzGauge.git` | 异地物理盘备份（`tools\backup-repo.cmd`） |
| `github` | `https://github.com/misakashiny/AtzGauge.git` | 云端私有仓库（`tools\gh-push.cmd`） |

`.gitignore` 是**白名单式**，只纳管约 1 MB 的「知识资产」
（`docs/` + `开发参考/` + `tools/` + `mods-atzgauge/` + 板型 + 从表 + README + LICENSE），
不跟踪 `build/` / `managed_components/` / `upstream/` / `backup/`。

---

## 许可与出处

- 基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（MIT）修改；本仓库**只含我们自己写的部分**
- 上游协议与文档归档在 `开发参考/xiaozhi-ai/`（版权归原作者）
- OBD 解析参考 [steveEcode/obd_brz_gauge](https://github.com/steveEcode/obd_brz_gauge)
- 本仓库自有代码沿用 MIT，见 [`LICENSE`](LICENSE)
