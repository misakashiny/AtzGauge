# 自动协议检测功能说明（归档）

> 来源：https://github.com/steveEcode/obd_brz_gauge/blob/main/docs/AUTO_PROTOCOL_DETECTION.md

## 功能概述

连接 OBD 设备时自动尝试协议 1-11，找到能工作的协议并保存，无需手动尝试。

## 使用方法（第一次使用推荐）

1. 设置 → OBD 协议 → 选 **"0 - Automatic"** → 提示 "Saved"
2. 进 BLE SCAN 页面连接 OBD 设备
3. 仪表自动尝试协议 1-11，每个协议发 `01 0C` 读转速测试，收到有效响应即停止并保存

## 工作原理

```
Protocol=0(Auto) → BLE 连接 → 初始化命令 → 循环协议 1-11：
  ATSP[n] → 01 0C → 有效 RPM？成功保存 NVS : 超时下一个
→ 检测完成 → 正常轮询
```

## 特点

- 自动保存 NVS，下次免检
- 每协议等 2 秒，总耗时 < 30 秒，通常 3-8 秒成功
- 检测期间不更新异常数据到显示
- 断开重连不自动重检（除非协议仍为 0）

## 日志示例

```
elm327_ble: === Starting protocol auto-detect ===
elm327_ble: [DETECT] Trying protocol 6...
elm327_ble: [DETECT] Protocol 6: SUCCESS! (RPM=800)
elm327_ble: [DETECT] Protocol 6: SUCCESS! Saving protocol 6 to NVS
```

## 何时手动选协议

1. 已知正确协议（阿特兹 = 协议 6）→ 直接选
2. 自动检测全失败 → 手动逐一测试（参考 BRZ_ZD8_PROTOCOL_GUIDE.md 的思路）
3. 连接不稳定 → 关自动检测，固定协议

## 协议列表

| 编号 | 描述 | 推荐车型 |
|---|---|---|
| 0 | 自动检测（推荐） | 所有 |
| 1 | SAE J1850 PWM | 美国车（Ford） |
| 2 | SAE J1850 VPW | 美国车（GM） |
| 3 | ISO 9141-2 (10.4k) | 欧洲车（旧） |
| 4 | ISO KWP2000 (5 baud) | 欧洲车 |
| 5 | ISO KWP2000 (fast) | 欧洲车 |
| **6** | **ISO 15765-4 CAN @500k** | **BRZ ZC6 / 马自达阿特兹** |
| 7 | ISO 15765-4 CAN @500k (29bit) | 现代车型 |
| 8 | ISO 15765-4 CAN @250k | 某些车型 |
| 9 | ISO 15765-4 CAN @250k (29bit) | 某些车型 |
| 10/11 | 标准/自适应 | 备选 |

## 常见问题

- 耗时：最多 22 秒，通常 3-8 秒
- 全失败：降级到协议 6 并输出日志
- 禁用：设置里选具体协议号
- 检测中看不到错误数据：只记录 RPM，不更新缓存
- 强制重检：协议设 0 → 断开 BLE → 重连
- 多设备可各自用不同协议（存本机 NVS）

## 调试

```bash
idf.py monitor -l DEBUG
idf.py monitor | grep elm327
idf.py monitor | grep "PROTOCOL_DETECT\|SUCCESS\|FAILED"
```
