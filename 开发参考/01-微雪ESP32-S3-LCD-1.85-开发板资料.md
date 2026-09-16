# 微雪 Waveshare ESP32-S3-LCD-1.85 开发板资料

> 来源：微雪官方中文文档 + 英文 Wiki（归档整理）
> 官方链接：https://docs.waveshare.net/ESP32-S3-LCD-1.85/Resources-And-Documents

---

## 一、核心规格

| 项目 | 参数 |
|---|---|
| 主控 | ESP32-S3R8（Xtensa 32 位 LX7 双核，最高 240MHz） |
| 无线 | 2.4GHz WiFi（802.11 b/g/n）+ Bluetooth 5（BLE），板载陶瓷天线 |
| 内存 | 512KB SRAM + 384KB ROM，**16MB Flash + 8MB PSRAM** |
| 屏幕 | 1.85 寸 TFT，**360×360** 分辨率，262K 色，控制器 **ST77916** |
| 触摸 | 电容触摸（CST816，**触摸版才有**），走**独立 I2C 总线**（见第三节） |
| 板载外设 | QMI8658 六轴传感器、PCF85063 RTC、PCM5101 音频解码、麦克风、TF 卡槽、锂电池充放电管理、扬声器接口 |
| 扩展接口 | UART（GPIO43/44）、I2C（GPIO10/11，不可做普通 GPIO） |
| 电源 | USB Type-C；3.7V 锂电池接口（MX1.25 2PIN） |
| 尺寸 | 49.95 × 48.08 mm（官方现行文档记 55.0 × 55.0 mm，含屏/结构件） |

⚠️ **分触摸版 / 非触摸版两种**，靠 **SKU** 区分：

| SKU | 型号 | 触摸 |
|---|---|---|
| **28514** | `ESP32-S3-Touch-LCD-1.85` | ✅ 有 |
| **28511** | `ESP32-S3-LCD-1.85` | ❌ 无 |

本项目（obd_brz_gauge）针对 **Touch 版** 开发（触控 UI + CST816 驱动），买/用之前先确认。
两个版本的**板载资源清单相同**（QMI8658、RTC、PCM5101、MIC、TF 卡槽、电池管理都在），
差别只在屏幕总成有没有贴触摸层 → **不能用"有没有 TF 卡槽"来判断是不是触摸版**。

> 📌 **TF 卡槽本项目用不到**：obd_brz_gauge 的全部持久化都在片上 flash（NVS + SPIFFS 分区：`theme_0` / `bootmedia`），
> 源码树里**没有任何 SD/MMC 驱动**。卡槽空着即可，插卡不会有任何效果。

## 二、LCD 引脚定义（重要）

| LCD 信号 | ESP32-S3 GPIO |
|---|---|
| LCD_SDA0 | GPIO46 |
| LCD_SDA1 | GPIO45 |
| LCD_SDA2 | GPIO42 |
| LCD_SDA3 | GPIO41 |
| LCD_SCK | GPIO40 |
| LCD_CS | GPIO21 |
| LCD_TE | GPIO18 |
| LCD_RST | EXIO2（经 TCA9554 扩展芯片） |
| LCD_BL（背光） | GPIO5 |

## 三、其他板载器件引脚

| 器件 | 引脚 |
|---|---|
| TF 卡 | MISO=GPIO16, MOSI=GPIO17, SCK=GPIO14, CS=EXIO3（D1/D2 = NC，**本项目不用**） |
| **触摸 CST816**（触摸版） | **SDA=GPIO1, SCL=GPIO3, INT=GPIO4, RST=EXIO1**（独立总线，不与 IMU/RTC 共用） |
| QMI8658 | SCL=GPIO10, SDA=GPIO11, INT1=EXIO5, INT2=EXIO4 |
| RTC PCF85063 | SCL=GPIO10, SDA=GPIO11, INT=GPIO9 |
| MIC | WS=GPIO2, SCK=GPIO15, SD=GPIO39 |
| 扬声器 PCM5101 | DIN=GPIO47, LRCK=GPIO38, BCK=GPIO48 |

> ⚠️ **触摸 I2C 引脚有一处资料冲突（实测备查）**：
> `obd_brz_gauge` 源码 V1（Waveshare）分支写的是 `I2C_Touch_SDA_IO=11 / SCL=10`（与 IMU/RTC **共用**总线），
> 微雪**现行**官方文档给的是 `GPIO1 / GPIO3`（**独立**总线），两者 INT/RST 一致（`GPIO4` / `EXIO1`）。
> 因此：**在 GPIO10/11 上扫不到 `0x15` 不能推出"非触摸版"**（那条线上是 RTC `0x51` + IMU `0x6A`）。
> 若烧完固件画面正常但触摸无反应，先怀疑引脚不匹配，用官方 Demo 复判。

## 四、开发环境要点

### Arduino 路线
- 开发板包：`esp32 by Espressif Systems` ≥ **3.0.2**（注意 3.x 基于 ESP-IDF v5.1，与旧版差异大）
- 板卡选 **ESP32S3 Dev Module**；USB 下载需开 **USB CDC On Boot**
- 库：LVGL **v8.3.10**（离线安装，在示例包里）、ESP32-audioI2S-master
- 示例程序：`ESP32-S3-LCD-1.85-Demo.zip`（LVGL_Arduino 演示板载设备）

### ESP-IDF 路线（本项目走这条）
- VSCode + Espressif IDF 插件 ≥ **5.3.1**（obd_brz_gauge 用 **5.5.3**）
- 常用命令：`idf.py set-target esp32s3` → `idf.py build` → `idf.py -p PORT flash monitor`
- 板载自动下载电路，一般不需要手动按 BOOT

### 注意事项（官方提醒）
- **电脑用户名必须是英文**，中文用户名会导致编译错误
- 若端口识别不了：按住 BOOT 键再插 USB 进入下载模式，下载完按 RESET
- 同时开板载全部外设（含音频）时 SRAM 占用大

## 五、出厂 Demo 验证清单（硬件到手先做）

1. 烧官方 Demo，屏幕亮、显示 360×360 UI ✅
2. 触摸滑动正常（**确认是触摸版**）✅
3. 旋钮/按键：本板无旋钮，靠 BOOT/PWR 键模拟（Demo 说明）✅

## 六、资料下载链接

- 官方文档（**新平台，含完整 LCD/触摸/TF 引脚表**）：https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85
- 原理图：https://www.waveshare.net/w/upload/7/76/ESP32-S3-LCD-1.85.pdf
- 示例程序：https://files.waveshare.net/wiki/ESP32-S3-LCD-1.85/ESP32-S3-LCD-1.85-Demo.zip
- ESP32-S3 芯片手册（中/英）：https://documentation.espressif.com/
- LVGL 文档：https://docs.lvgl.io/master/getting_started/index.html
