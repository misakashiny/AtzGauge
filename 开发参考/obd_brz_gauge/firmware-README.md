# 预编译固件与烧录地址（归档）

> 来源：https://github.com/steveEcode/obd_brz_gauge/blob/main/firmware/README.md

## 文件

| 文件 | 说明 |
|---|---|
| `release/bootloader/bootloader.bin` | Bootloader |
| `release/partition_table/partition-table.bin` | 分区表 |
| `release/ota_data_initial.bin` | OTA 数据初始分区 |
| `release/obd_brz_gauge.bin` | 应用固件 |
| `release/bootmedia.bin` | 开机动画（SPIFFS 分区，可跳过） |
| `release/latest.json` | 发布清单（App 检测新固件用） |
| `release/flash_address_map.txt` | 烧录地址参考 |

## 烧录地址

### main 分支

| 地址 | 文件 |
|---|---|
| `0x0` | bootloader.bin |
| `0x8000` | partition-table.bin |
| `0xf000` | ota_data_initial.bin |
| `0x20000` | obd_brz_gauge.bin |
| `0x620000` | bootmedia.bin |

### theme-upgrade 分支

| 地址 | 文件 |
|---|---|
| `0x0` / `0x8000` / `0xf000` / `0x20000` | 同上 |
| `0x620000` | theme_0.bin（可选，4MB 主题分区） |
| **`0xA20000`** | bootmedia.bin（⚠️ 地址变了） |

## 一键烧录命令（main 分支）

```bash
esptool.py --chip esp32s3 -p PORT -b 460800 write_flash \
  0x0 release/bootloader/bootloader.bin \
  0x8000 release/partition_table/partition-table.bin \
  0xf000 release/ota_data_initial.bin \
  0x20000 release/obd_brz_gauge.bin \
  0x620000 release/bootmedia.bin
```

theme-upgrade 分支 bootmedia 改 `0xA20000`。

## 注意

- 全部从当前源码树构建
- bootmedia 可跳过（无开机动画）
- latest.json 供配套 App 校验固件
- Windows 下需要 Python + `pip install esptool`
