# Waveshare ESP32-S3-Touch-LCD-1.85 (AtzGauge voice slave)

微雪 ESP32-S3-Touch-LCD-1.85（SKU 28514）的 **AtzGauge 定制板型**。

## 与官方 `esp32-s3-touch-lcd-1.85` 板型的关系

**硬件完全相同，引脚一个都没改**（`config.h` 与官方板型逐字节一致）。
唯一区别：本板型在启动时会拉起 **ESP-NOW 从表**（`main/espnow_slave/`），
接收仪表主表广播的车况。

### 为什么另起一个板型，而不是改官方那个

小智的 `AGENTS.md` 与 `docs/custom-board.md` 都明确要求：

> Never alter an existing board's pins to support different hardware. Add a uniquely
> named board or release variant; **board identity affects OTA compatibility.**

具体到本项目：固件上报的 `type`/`name` 决定 **OTA 升级通道**。若沿用
`esp32-s3-touch-lcd-1.85`，设备在官方 OTA 推送时会被**官方固件静默覆盖**，
我们的 ESP-NOW 从表就没了。所以板型标识必须独立。

## 编译

**★ 板型参数必须是 `厂商/目录名`**，只写目录名会被 `build.py` 拒绝
（`_find_board_config_candidates()` 拿 `set(BOARD_DIR "厂商/目录")` 做全等比较）：

```powershell
# 执行策略禁止直接 dot-source 脚本，所以经 tools\idf-run.ps1 激活 ESP-IDF
powershell -NoProfile -ExecutionPolicy Bypass -File D:\AtzGauge\tools\idf-run.ps1 `
    -Command "python scripts/build.py waveshare/esp32-s3-touch-lcd-1.85-atzgauge"
```

产物：`build/merged-binary.bin`（烧录地址 `0x0`）

烧录与抓日志（`D:\AtzGauge\tools\`）：

```powershell
# 烧录：别用 @flash_args（嵌套引号在 Windows 上无法可靠传递），用这个批处理
powershell -NoProfile -ExecutionPolicy Bypass -File D:\AtzGauge\tools\idf-run.ps1 `
    -Command "cmd /c D:\AtzGauge\tools\flash-atzgauge.bat"

# 抓完整启动日志：这个脚本自己脉冲 RTS 复位，所以能抓到第一行
# （car-log.ps1 只能 attach，看不到启动横幅；且两者都会独占 COM3，不能同烧录并行）
powershell -NoProfile -ExecutionPolicy Bypass -File D:\AtzGauge\tools\boot-log.ps1 -Port COM3 -Seconds 70
```

## 运行行为

1. 正常启动小智，进入配网/联网流程
2. 后台任务等 **STA 真正连上路由器**（信道确定）后，自动初始化 ESP-NOW
3. 启动时会打印一行自检结论：

```
I (xxxx) espnow_slave: SELF-TEST PASS: packet -> cache path verified (8 fields)
```

这一行是**阶段 A 的证据**：它注入一个已知数值的合成包，再读回车况缓存逐项比对，
证明「收包 → 解析 → 车况缓存」这段逻辑正确，**不需要第二块板**。

4. 收到主表数据后，约 1Hz 打印一行车况：

```
I (xxxx) espnow_slave: [#10 seq=1247 gaps=0] RPM 3210 | SPD 87 km/h | COOL 91 C | OIL 104 C | ...
```

## 点屏开始对话（触摸唤醒）

点一下屏幕 = 开始对话。**只在空闲状态生效**：AI 正在回答时点屏一律忽略，
避免误触打断（刻意的产品选择，不是漏做）。

实现要点（完整踩坑见 `D:\AtzGauge\开发参考\17-会说话的从表-实现与验证状态.md` 第十节）：

- **不接 LVGL**：独立任务按 30ms 轮询 CST816S 数据寄存器 `0x02`（5 字节），
  因此不需要 LVGL 锁，也不碰任何样式绑定
- 读失败一律当作"未按下"，**绝不** `ESP_ERROR_CHECK`（上一轮无限重启的根因）
- 用原生 `i2c_master`，不用 `esp_lcd_panel_io`：后者会把 CST816S 休眠时的正常 NACK
  打成 ERROR —— 实测 30ms 轮询下 **45 秒刷 1094 行**
- 启动时**两条候选总线都探测**（config.h 写的独立 I2C_NUM_1，以及 TCA9554 那条共享
  总线 —— 1.85 系列不同批次接法不同），谁应答用谁；都不应答就打印一行并**整体禁用**
  本功能，不留空轮询
- 探测读的是 **CHIP_ID `0xA7`**（未触摸也必须应答），并带 10×100ms 退避重试

启动日志判据：

```
I (1168) waveshare_lcd_1_85: CST816S found on dedicated bus (config.h TP_*) (chip id 0xB5, attempt 1)
I (6678) waveshare_lcd_1_85: touch tap -> start conversation
I (6678) StateMachine: State: idle -> connecting
I (6748) StateMachine: State: connecting -> listening
```

若打印的是 `touch controller not found on any bus; tap-to-talk disabled`，说明触摸控制器
没有应答 I2C：**先检查触摸 FPC 排线**（屏幕模组背面较窄那条）是否插到位，再做一次真断电
（拔 USB 等 10 秒，按复位键不算）。需要逐次计数时，把源码里的 `TP_DIAG` 置 1 重新编译，
日志会每 5 秒打一行 `touch diag: ok=… fail=…`。

## ⚠️ 信道硬约束（收不到包先看这条）

仪表主表把 ESP-NOW 信道**硬编码为 1**。从表这边信道**跟随 STA**，
所以 **路由器/热点必须工作在信道 1**，否则一个包都收不到。

启动日志会直接告诉你当前信道对不对：

```
I (xxxx) espnow_slave: ESP-NOW slave up; following STA channel 1 (master broadcasts on ch1)
```

若打印的是 `STA is on channel 6 but the gauge master broadcasts on channel 1`，
去路由器后台把信道改成 1。

### 收不到包时怎么判断是「信道不对」还是「主表不在」

这两种故障以前无法区分，只能靠猜。现在从表会把「收到了帧但解析失败」计数并记录
来源 MAC，给出方向性判据：

```
W (xxxx) espnow_slave: no master packet yet, but 37 foreign frame(s) arrived
                       (last: 128 bytes from aa:bb:cc:dd:ee:ff)
W (xxxx) espnow_slave: -> radio is alive on channel 6 while the gauge master transmits on
                       channel 1. Set the router/hotspot to channel 1 (or move the master
                       to channel 6).
```

| 现象 | 判断 |
|---|---|
| 出现 `foreign frame(s)` 但没有有效包 | 射频是活的，收到的是别人的流量 → **大概率信道不对** |
| 完全没有 `foreign frame(s)` | 这个信道上根本没有 ESP-NOW 流量 → **主表没开或太远** |

同一判据也接进了 MCP 工具 `self.car.get_status`：语音问"水温多少"时，若链路没数据
但收到了外来帧，模型会直接说"多半是信道不对"，而不是笼统地回答没有数据。

## 已知取舍

| 项 | 状态 | 说明 |
|---|---|---|
| presence 心跳 | **默认关闭** | 主表靠它统计在线从表数，并据此决定是否播放三连表开机动画。本板是小智板、没有仪表位置，让主表误判会引起不必要的联动。要参与联动时把 `ESPNOW_SLAVE_SEND_PRESENCE` 改成 1 |
| 主表 MAC 绑定 | 未启用 | 阶段 A 接受任何主表。`espnow_slave_bind_master()` 接口已留出（仅作用于内存），后续接 Settings 即可 |
| 自检 | 默认开启 | 台架验证用，正式使用可把 `ESPNOW_SLAVE_SELF_TEST` 设为 0 |
| 档位 / 里程 | 未移植 | 依赖 `vehicle_profiles` 与 `nvs_storage` 两个大子系统，AI 告警不需要 |

## 尚未验证（阶段 B）

射频那一段**还没验证过**——需要第二块板跑仪表主表固件，才能确认真实的
ESP-NOW 广播能被本板收到。阶段 A 只证明了收包之后的逻辑正确。

已经验证过的（完整证据见 `D:\AtzGauge\开发参考\17-会说话的从表-实现与验证状态.md`）：

| 项 | 证据 |
|---|---|
| 收包 → 解析 → 车况缓存 | `SELF-TEST PASS: packet -> cache path verified (8 fields)` |
| 阈值规则 + 本地音频告警 | `SELF-TEST PASS: rule engine + local audio alert path OK` + `Application: Alert [...]` 真出声 |
| MCP 语音问答工具 | `MCP: Add tool: self.car.get_status` |
| 信道对齐（方案①） | `ESP-NOW slave up; following STA channel 1 (master broadcasts on ch1)` |
| 点屏开始对话（触摸链路） | 第一版固件实测：`CST816S answered (finger_num=0)` + 两次 `touch tap -> start conversation` → `idle → connecting → listening`（控制器随后不再应答 I2C，见上节） |

`obd_brz_gauge/firmware/release/` 里已备好主表预编译固件，再买一块同型号板即可完成阶段 B。
