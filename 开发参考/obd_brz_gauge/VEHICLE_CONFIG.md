# 车型配置开发指南（归档）

> 来源：https://github.com/steveEcode/obd_brz_gauge/blob/main/docs/VEHICLE_CONFIG.md
> ★ 给阿特兹新增车型 profile 就看这篇

## 概述

数据驱动的车型配置架构。**新增车型只需编辑 2 个文件**，不改任何解析逻辑代码：

```
默认行为 = OBD2 标准协议 (SAE J1979)
只有在 vehicle_custom_config.h 里声明了的车型，才使用自定义 CAN 规则或油温公式。
```

## 文件结构

| 文件 | 作用 |
|---|---|
| `vehicle_profiles.c` | 车型基础参数（传动比/档位/轮胎） |
| `vehicle_custom_config.h` | 自定义覆盖（CAN 规则/油温公式/协议） |
| `vehicle_profiles.h` | 结构体定义 + API |

## 添加新车型

### 步骤 1：基础参数（vehicle_profiles.c 的 s_profiles[] 末尾）

```c
{
    .name = "My Car",                    // 显示名称
    .final_drive_ratio = 3.73f,          // 主减速比
    .tire_rolling_radius_m = 0.310f,     // 轮胎滚动半径(m)
    .gear_count = 6,                     // 前进挡数
    .gear_ratios = {0, 3.63f, 2.38f, 1.56f, 1.18f, 1.00f, 0.81f},
    .gear_tolerance = 0.15f,             // 档位识别容差
    // 其余字段可省略，默认 = 纯 OBD2 标准
},
```

**只要标准 OBD2 的话，到这里就结束！** 不需要步骤 2。

### 步骤 2：自定义覆盖（可选，vehicle_custom_config.h）

#### 2a. CAN 广播解码规则

```c
static const can_rule_t can_rules_mycar[] = {
    // CAN_ID  位偏移  位长  乘数         偏移    通道
    { 0x140,   16,    14,   1.0f,       0.0f,  CH_RPM },
    { 0x140,   48,     8,   100.0f/255, 0.0f,  CH_TPS },
    { 0x360,   16,     8,   1.0f,     -40.0f,  CH_OIL_TEMP },
    { 0x360,   24,     8,   1.0f,     -40.0f,  CH_COOLANT },
};
```

位域：`bit_off` 从 LSB 开始（Byte0 bit0=0, Byte1 bit0=8）；`bit_len` 位长；
最终值 = raw × scale + offset。
可用通道：`CH_RPM, CH_SPEED, CH_OIL_TEMP, CH_COOLANT, CH_TPS, CH_LOAD, CH_INTAKE, CH_BOOST`

#### 2b. 油温公式

```c
// 标准 PID 01 5C
static const oil_formula_t oil_std = { OIL_STD_PID, {0x5C}, 1, 0, 1, 1.0f, -40.0f, 0 };
// UDS Mode 22 单字节
static const oil_formula_t oil_uds_1byte = { OIL_UDS_22, {0x11,0x1F}, 2, 0, 1, 1.0f, -50.0f, 0 };
// UDS Mode 22 双字节大端
static const oil_formula_t oil_uds_2byte = { OIL_UDS_22, {0x13,0x10}, 2, 0, 2, 0.01f, -40.0f, 0 };
```

| 类型 | 说明 | 命令格式 |
|---|---|---|
| `OIL_STD_PID` | 标准 Mode 01 | `01 XX\r` |
| `OIL_UDS_22` | UDS Mode 22 | `22 XX XX\r` |
| `OIL_SPECIAL` | 特殊解析（需额外代码） | 自定义 |

#### 2c. 注册覆盖（s_vehicle_overrides[]）

```c
{
    .match_name      = "My Car",              // 与 profiles 里 name 完全一致
    .can_rules       = can_rules_mycar,       // NULL = 不用 CAN
    .can_rule_count  = 4,
    .oil_primary     = &oil_uds_1byte,        // NULL = 标准 01 5C
    .oil_secondary   = &oil_std,              // 主公式连续失败后回退
    .forced_protocol = 6,                     // ELM327 协议（0 = 自动）
    .functional_addr = false,                 // true = ATSH7DF（BMW 等）
    .obd_timeout     = 0x0F,                  // ATST 超时（0 = 默认 0x19）
    .has_boost       = false,                 // 涡轮车 = true
    .poll_gap_ms     = 1,                     // 轮询间隔 ms（0 = 默认 30ms）
},
```

## 完整示例：Mazda MX-5 ND（仓库已有，作参考）

```c
// vehicle_profiles.c
{
    .name = "MX-5 ND",
    .final_drive_ratio = 2.866f,
    .tire_rolling_radius_m = 0.300f,
    .gear_count = 6,
    .gear_ratios = {0, 5.087f, 2.991f, 2.035f, 1.594f, 1.286f, 1.000f},
    .gear_tolerance = 0.15f,
},

// vehicle_custom_config.h
static const oil_formula_t oil_mazda_1310 = { OIL_UDS_22, {0x13,0x10}, 2, 0, 2, 0.01f, -40.0f, 0 };
static const oil_formula_t oil_mazda_111f = { OIL_UDS_22, {0x11,0x1F}, 2, 0, 1, 1.0f, -50.0f, 0 };

// s_vehicle_overrides[]
{
    .match_name  = "MX-5 ND",
    .oil_primary = &oil_mazda_1310,
    .oil_secondary = &oil_mazda_111f,
    .obd_timeout = 0x0A,
    .poll_gap_ms = 1,
},
```

## 运行时查找逻辑

```
vehicle_profile_get_override()
    ├── 找到 override → 用自定义 CAN 规则/油温公式
    └── NULL          → 纯 OBD2 标准（01 0C/0D/05/5C/0F/04/11/42）
```

## CAN 规则解析 API

```c
bool can_extract_bits_le(const uint8_t data[8], uint8_t bit_off, uint8_t bit_len, uint32_t *out);
void can_apply_rules(const can_rule_t *rules, uint8_t count, uint16_t can_id,
                     const uint8_t data[8], float channels[CH_COUNT]);
const char *oil_formula_build_cmd(const oil_formula_t *f, char *buf, size_t buflen);
int16_t oil_formula_parse_resp(const oil_formula_t *f, const uint32_t *resp_data, uint8_t resp_len);
```

## 注意事项

1. `match_name` 必须与 profiles 里 `name` **字符串完全一致**
2. `OIL_SPECIAL` 用于丰田 Mode 21 等，复杂逻辑仍在 elm327_ble_client.c
3. 位偏移用 SAE J1939 风格：Byte0 LSB = bit 0，Byte1 LSB = bit 8
4. 双字节公式按大端序：`value = data[byte] * 256 + data[byte+1]`
5. `oil_primary` 连续失败 5 次自动切 `oil_secondary`
