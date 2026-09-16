# OBD BRZ Gauge（项目主 README 归档）

> 来源：https://github.com/steveEcode/obd_brz_gauge （main 分支）
> 归档用途：马自达阿特兹仪表项目参考

A round ESP-IDF car gauge for the Waveshare ESP32-S3-Touch-LCD-1.85. It connects
to an ELM327-compatible BLE OBD adapter, reads vehicle data, and renders a
touch UI with LVGL.

基于 ESP-IDF 的圆形车载仪表，硬件为微雪 Waveshare ESP32-S3-Touch-LCD-1.85。
通过 BLE 连接兼容 ELM327 的 OBD 适配器读取车辆数据，用 LVGL 渲染触控界面。

> 本项目基于 [zhaizhaitao/open_obd_dsp](https://github.com/zhaizhaitao/open_obd_dsp)
> 二次开发。本仓库新增了多车型适配、三连表联动和主题系统。

---

## ⚠️ 分支说明

- **`main`** — 稳定分支，主题编译进固件
- **`theme-upgrade`** — 实验分支，支持运行时加载主题（4MB 主题分区）
- 两个分支分区布局不兼容，**无法 OTA 互升**。建议先用 `main`。

## 当前状态

| | |
|---|---|
| 硬件 | Waveshare ESP32-S3-Touch-LCD-1.85 (360×360, 16MB flash, 8MB PSRAM) |
| 软件栈 | ESP-IDF 5.5.3、LVGL 8 |
| 链路 | BLE + ELM327（标准 OBD PID；仅 ZN/C6 CAN 走 ATMA 监听） |
| 三连表 | 一主多从，ESP-NOW 联动 |
| 已验证 | Subaru BRZ ZN/C6（其余车型已配置，部分待上车验证） |

**内置车型（12 个）**：`OBD2 Generic` · `ZN/C6 CAN` · `ZN/C6 PID` · `ZD8 OBD` · `ZD8` · `MX-5 ND` · `BMW F/G` · `BMW G OBD` · `JCW F56` · `POS 997.2` · `POS 997.1` · `GIULIA 2.0T`

## 主要特性

- BLE 扫描连接 ELM327 兼容 OBD 设备
- ELM327 单线程轮询（避免适配器抢占）；仅 ZN/C6 CAN 用 ATMA
- 实时显示：转速、车速、水温/进气温/机油温、机油压力、涡轮压力、节气门、负荷、电压、档位
- 除标准 PID 01 5C 外的厂商油温：丰田/斯巴鲁 Mode 21、**马自达 Mode 22**、MINI/宝马 Mode 22 等
- 数据中断自愈：上车通电无需手动重连
- 转速超限闪烁报警、刹车温度/油压报警节流（30 秒一次）
- 三连表（ESP-NOW）：主表读 OBD 广播，从表零额外 OBD 负载
- 数据驱动主题：一个文件夹 + 一份清单，不用写 C
- BLE 设备清单（GATT 服务）：App 刷写前做硬件匹配校验
- 自定义开机图/动画（bootmedia SPIFFS 分区）
- 用户配置存 NVS

## 快速开始

### 从源码编译

```bash
git clone https://github.com/steveEcode/obd_brz_gauge.git
cd obd_brz_gauge
git checkout theme-upgrade      # 或留在 main
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

### 烧录预编译固件（main 分支）

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 firmware/release/bootloader/bootloader.bin \
  0x8000 firmware/release/partition_table/partition-table.bin \
  0xf000 firmware/release/ota_data_initial.bin \
  0x20000 firmware/release/obd_brz_gauge.bin \
  0x620000 firmware/release/bootmedia.bin
```

（theme-upgrade 分支的 bootmedia 在 `0xA20000`，主题分区在 `0x620000`）

## 目录结构

| 路径 | 内容 |
|---|---|
| `main/app_main.c` | 入口：硬件、LVGL、BLE 和任务启动 |
| `main/app_obd_dsp` | OBD 数据缓存、车辆配置、CAN 解码、开机动画 |
| `main/bsp_obd_dsp` | 板级支持：BLE、NVS、LCD、触摸、I2C、IO 扩展器、ESP-NOW |
| `main/export_path` | LVGL UI（SquareLine 导出）+ 主题框架 |
| `themes` | 主题清单与素材 |
| `bootmedia` | 开机动画块（SPIFFS 分区源） |
| `tools` | 辅助脚本（图片转换、开机 Block 构建等） |
| `firmware/release` | 预编译固件 |
| `model` | 3D 打印外壳、表座、支架 |
| `docs` | 文档 |

## 3D 模型（开源）

| 文件 | 说明 |
|---|---|
| `model/esp32_1.85_weixue/housing.stl` | 开发板外壳（**微雪 1.85 板专用**） |
| `model/Subaru/brz_zc_n6/triple_gauge_pod.stp` | BRZ 三连表底座 |
| `model/mazda/mx5_nd/air_vent_bracket.stl` | MX-5 ND 出风口支架 |

## 适配提醒

- 换开发板 → 重点检查 `main/bsp_obd_dsp`（引脚、屏幕参数）
- 换车型/适配器 → 重新验证 BLE 服务、PID 与解析（见 VEHICLE_CONFIG.md）

## 开源协议

**GPLv3**。可以自由使用修改；若分发修改版本，必须同样以 GPLv3 开源。

## 致谢

- Hokori23（性能优化贡献）
- timurrrr/ft86（FT86 CAN 总线文档，使 CAN 广播监听成为可能）
