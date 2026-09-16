# OBD BRZ Gauge 中文说明（归档）

> 来源：https://github.com/steveEcode/obd_brz_gauge/blob/main/docs/README.zh-CN.md

## 项目简介

基于 ESP-IDF 的车载圆形仪表，运行在微雪 Waveshare ESP32-S3-Touch-LCD-1.85 上。
通过 BLE 连接 ELM327 兼容 OBD 适配器读取车辆数据，LVGL 渲染触控界面。

## 当前状态

- 硬件：微雪 ESP32-S3-Touch-LCD-1.85
- 软件栈：ESP-IDF 5.5.3、LVGL 8
- 链路：BLE + ELM327（标准 OBD PID；只有 ZN/C6 CAN 用 ATMA）
- 内置车型 12 个：OBD2 Generic、ZN/C6 CAN、ZN/C6 PID、ZD8 OBD、ZD8、MX-5 ND、BMW F/G、BMW G OBD、JCW F56、POS 997.2、POS 997.1、GIULIA 2.0T
- 三连表：一主多从 ESP-NOW 联动

## 功能概览

- BLE 扫描连接 ELM327 OBD 设备
- ELM327 单线程轮询；仅 ZN/C6 CAN 走 ATMA，其余全部 OBD-only
- 自定义开机图/动画（bootmedia SPIFFS 分区）
- 统一车辆配置系统（`vehicle_custom_config.h` 管理阈值/报警/表盘范围）
- 实时显示：转速、车速、水温/进气温/机油温、机油压力、涡轮压力、节气门、负荷、电压、档位（优先 CAN 精确档位，回退转速/车速估算）
- 断线显示 "NO SIGNAL"
- 车型选择：各车型独立传动比（最高 8 挡）、油温策略、涡轮增压、按车型锁定协议
- 厂商油温读取：丰田/斯巴鲁 Mode 21、**马自达 Mode 22**、MINI/宝马 Mode 22、宝马 F 系 Mode 22 44 02
- 可配置指针表盘页，下滑切换数据源
- 转速超限闪烁报警；刹车温度/油压报警节流（30 秒一次）
- 三连表开机动画（ESP-NOW 同步 "RACE / AS / ONE"）
- 连接自愈：BLE 通知订阅完成后再初始化、每次重连重新初始化、数据中断自愈——上车通电无需手动重连
- 三连表：主表读 OBD 广播（`SkyGauge-XXYY` 带后缀防串扰），从表未配对时进 "FIND MASTER" 扫描页
- 配置存 NVS；里程/行程统计仅运行时内存累计

## 三连表说明

一个 ELM327 蓝牙适配器只允许一个客户端连接。一块板作主表（保持 BLE + ELM327），其余作从表，
主表通过 ESP-NOW 广播解析后的数据缓存。三块板烧同一份固件，设置页 → MULTI-GAUGE 选角色。
配对走真蓝牙：主表广播 `SkyGauge-XXYY`，从表扫描选择。

## 目录说明

- `main/app_main.c`：入口，初始化硬件、LVGL、UI、BLE 任务
- `main/app_obd_dsp`：OBD 数据缓存、车辆配置、CAN 解码、里程统计、开机动画
- `main/bsp_obd_dsp`：BLE 客户端、NVS、LCD、触摸、I2C、IO 扩展器、ESP-NOW
- `main/export_path`：SquareLine 导出的 UI 代码、字体、图片
- `bootmedia`：开机动画媒体块
- `model`：3D 打印模型
- `tools`：辅助脚本
- `firmware/release`：预编译固件

## 依赖环境

- ESP-IDF 5.5.3 或更高
- Python 环境与 ESP-IDF 工具链
- 兼容 ELM327 的 BLE OBD 适配器

组件依赖（`main/idf_component.yml`）：lvgl/lvgl、espressif/esp_lcd_touch、espressif/button、espressif/knob

## 编译与烧录

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

预编译固件烧录（main 分支）：

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 write_flash \
  0x0 firmware/release/bootloader/bootloader.bin \
  0x8000 firmware/release/partition_table/partition-table.bin \
  0xf000 firmware/release/ota_data_initial.bin \
  0x20000 firmware/release/obd_brz_gauge.bin \
  0x620000 firmware/release/bootmedia.bin
```

## 开发与适配说明

1. 项目针对微雪 ESP32-S3-Touch-LCD-1.85 做板级适配
2. 换屏幕/触摸芯片/IO 扩展器/引脚 → 重点检查 `main/bsp_obd_dsp`
3. 换车/适配器 → 重新验证 BLE 服务、特征值、命令格式、返回解析
4. UI 资源在 `main/export_path`

## 已知限制

- 重点验证了 Subaru BRZ ZC6
- 不保证所有 ELM327 设备都稳定
- 不保证所有车型 PID 与返回格式一致
- 不同版本 ESP-IDF 可能需小幅调整 BSP/依赖

## 3D 模型

| 文件 | 说明 |
|---|---|
| `model/esp32_1.85_weixue/housing.stl` | 开发板外壳 |
| `model/Subaru/brz_zc6/triple_gauge_pod.stp` | BRZ 三连表底座 |
| `model/Subaru/brz_zc6/passenger_dashboard_scan.stl` | 副驾仪表台扫描件 |
| `model/mazda/mx5_nd/air_vent_bracket.stl` | MX-5 ND 出风口支架 |
