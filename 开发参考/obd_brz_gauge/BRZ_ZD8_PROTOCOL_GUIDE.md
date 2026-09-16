# BRZ ZD8 OBD 协议快速诊断（归档·换车找协议的思路模板）

> 来源：https://github.com/steveEcode/obd_brz_gauge/blob/main/docs/BRZ_ZD8_PROTOCOL_GUIDE.md
> 阿特兹预期协议 = 6（ISO 15765-4 CAN@500K），若读不到数据按本文思路排查。

## 症状

- ✗ BLE 已连接
- ✗ 所有参数显示 0
- ✗ 车型已选定

## 快速修复步骤

1. 设置里依次尝试协议 1-11（每次切换后重新连接 OBD）
   - 协议 5：ISO 15765-4 CAN@250K
   - **协议 6：ISO 15765-4 CAN@500K（现代车最常见，阿特兹预期）**
   - 协议 7：CAN@arbitrary 等
2. 判断成功标准：RPM 非 0、Speed 有变化、水温油温合理
3. 找到正确协议后报告，供项目添加预设

## 深层诊断

### 验证 OBD 设备本身
手机装 Torque Pro / Car Scanner，连同一设备，扫 `01 0C`：
- 手机能读 → 设备正常，问题在仪表
- 手机也读不到 → 适配器故障或与车不兼容

### 检查仪表日志（idf.py monitor）
```
elm327_ble: Send 01 0C
elm327_ble: RAW[...]: 41 0C 12 34     ← 正常
elm327_ble: Parsed RPM: 4660
```
异常日志：`PARSE_FAIL` / `TIMEOUT_AGAIN`

## 通用 OBD-II PID 速查

| 命令 | 含义 | 单位 |
|---|---|---|
| `01 0C` | RPM | rpm（÷4） |
| `01 0D` | 车速 | km/h |
| `01 05` | 水温 | °C（A-40） |
| `01 0F` | 进气温度 | °C（A-40） |
| `01 04` | 发动机负荷 | % |
| `01 11` | 节气门 | % |
| `01 42` | 电压 | V |

只要有一个 PID 能读（如 RPM），协议就是对的，其余问题在个别 PID 支持上。

## 记录发现

- 车型 + OBD 型号 + 协议号 = 工作组合
- 可用 PID 列表
- 特殊响应格式
