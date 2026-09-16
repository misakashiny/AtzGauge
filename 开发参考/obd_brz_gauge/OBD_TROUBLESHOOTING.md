# OBD 数据接收故障排查指南（归档）

> 来源：https://github.com/steveEcode/obd_brz_gauge/blob/main/docs/OBD_TROUBLESHOOTING.md

## 第一步：确认 BLE 物理连接

1. 仪表进 **BLE SCAN** 页面，看 OBD 设备是否出现，记下完整设备名
2. 不出现 → 确认 OBD 设备已插好、通电、未与其他设备配对占用；重启 OBD 设备
3. 连接后：版本页顶部显示连接状态；串口日志出现 `elm327_ble: OBD BLE connected`
4. 连不上 → 确认设备是 **BLE**（不是 Classic）；先用手机连一次验证设备正常

## 第二步：验证协议连接

连接后仪表自动发初始化命令：

```
ATZ（复位）ATE0（关回显）ATL0 ATS1 ATH0 ATAT1 ATST 19 ATSP? ATSH7E0  01 00（能力探测）
```

- 失败迹象：日志持续 "Send" 无回应、反复重试初始化、进不了轮询
- 解决：检查协议（**马自达阿特兹 = 协议 6，ISO 15765-4 CAN@500K**）；换协议重连；用手机 OBD 软件验证设备

## 第三步：检查数据轮询和解析

正常应循环查询：`01 0C`（RPM）、`01 0F`（进气温度）、`01 0D`（车速）、`01 05`（水温）、`01 04`（负荷）、`01 11`（节气门）、`01 42`（电压）、油温（车型相关，马自达 = Mode 22）。

错误数据标记：

| 参数 | 错误标记 | 含义 |
|---|---|---|
| RPM | 0 | 从未收到有效数据 |
| Speed | 0 | 未收到或车速为 0 |
| Oil Temp | -100 | 未响应或设备不支持 |
| Load / TPS | -1 | 从未收到有效数据 |

用手机 App（Torque/Car Scanner）验证同一批 PID 是否可用、回应格式是否标准。

## 第四步：RS485 刹车温度（独立，本项目可忽略）

- GPIO13(TX)/12(RX)，9600 baud，Modbus RTU
- 状态：OK / PROBE / TIMEOUT / PARSE FAIL
- 诊断脚本：`python3 python_quick_rs485_check.py --baud 9600 --tries 5`

## 第五步：综合检查

1. 断电仪表 → 重启 OBD 设备 → 重开仪表 → BLE SCAN 重连
2. 检查 NVS 配置：协议、BLE 设备名、亮度主题
3. 恢复出厂：开发者菜单 → 清除 NVS

## 第六步：调试日志

```bash
idf.py menuconfig        # Component config → Log output → DEBUG
idf.py monitor
```

关键日志标签：`elm327_ble:`（BLE 与数据）、`obd_data_cache`（数据缓存）、`ERROR`/`WARN`

## 常见问题

- **全部显示 0**：轮询未开始或设备未初始化；查 BLE 状态与初始化日志
- **能连但收不到新数据**：查协议（阿特兹 = 6）；手机 App 验证；重连或换设备
- **转速车速正常但油温/负荷读不到**：该 PID 车型不支持，需不同查询方式
- **判断设备是否故障**：手机连同一设备，手机也读不到 = 设备问题
