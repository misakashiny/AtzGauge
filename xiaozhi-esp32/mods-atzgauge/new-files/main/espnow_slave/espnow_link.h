// espnow_link.h -- ESP-NOW 从表接收（从 obd_brz_gauge 移植）
//
// 角色：本设备是「会说话的从表」。仪表主表以 10Hz 广播车况，本模块收包后写入
//       obd_data_cache，供 AI 阈值判断 / 串口打印 / TTS 播报读取。
//
// 【与上游 espnow_link.c 的关键差异】
//   1. 不初始化 WiFi。上游的 wifi_espnow_init() 会 esp_wifi_set_mode/start/
//      set_channel，但小智固件的 WiFi 由 WifiBoard 掌管并且必须连上路由器上云。
//      本模块只调用 esp_now_init() 并注册接收回调。
//   2. 因此也【不设置信道】。ESP-NOW 走 STA 接口，自动跟随 STA 所在信道；
//      STA 连上路由器后即为该路由器的信道。
//      ★ 由此推出硬约束：仪表主表把信道硬编码为 1（espnow_link.c ESPNOW_CHANNEL），
//        所以路由器/热点必须工作在【信道 1】，否则收不到任何包。
//   3. 去掉主表路径、联动测试、UI 同步、ELM327 依赖、NVS 存储依赖。

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 启动从表：初始化 ESP-NOW 并注册接收回调。
// 必须在 WiFi STA 已启动之后调用（小智里即网络就绪之后）。
// 可重复调用，重复调用为空操作。
// 返回 true 表示 ESP-NOW 已就绪。
bool espnow_slave_start(void);

// 推荐入口：起一个后台任务，等 STA 真正连上 AP（信道确定）后再调用
// espnow_slave_start()。不阻塞调用者（板级构造函数里可以直接调）。
// 这一点很关键：ESP-NOW 跟随 STA 信道，STA 未连接时信道是不确定的。
void espnow_slave_start_async(void);

// 是否收到过主表数据，且距今未超过超时（默认 2s）
bool espnow_slave_has_data(void);

// 最近一次收到主表数据距今的毫秒数；从未收到返回 -1
int64_t espnow_slave_last_rx_age_ms(void);

// 累计收到的有效车况包数（用于诊断丢包 / 验证通路）
uint32_t espnow_slave_rx_count(void);

// 累计收到但解析失败的帧数（长度/魔数/版本不符）。
// 用途：区分两种「收不到数据」——
//   >0 且长时间没有有效包 → 射频活着但收到的是别人的流量 → 大概率信道不对
//   恒为 0               → 这个信道上根本没有 ESP-NOW 流量 → 主表没开或太远
uint32_t espnow_slave_rx_mismatch_count(void);

// 最近听到的主表名字（空字符串 = 尚未收到）
const char *espnow_slave_master_name(void);

// 绑定主表 MAC（全零 = 不限制，接受任何主表）。默认全零。
// 留给后续通过设置页/BLE 配对实现；阶段 A 不启用。
void espnow_slave_bind_master(const uint8_t mac[6]);
const uint8_t *espnow_slave_get_bound_master_mac(void);

// 阶段 A 验证用：把一个合成车况包直接喂进接收链路（不经过射频）。
// 用来在没有第二块板的情况下证明「收包 → 车况缓存」这段逻辑正确。
// 传入的必须是主表格式的 espnow_obd_packet_t 原始字节。
// 返回 true 表示被接受（magic/version 校验通过）。
bool espnow_slave_inject_packet(const uint8_t *data, int len);

// 构造一个测试包（magic/version/name 填好，数值由调用方给）。
// 仅供 espnow_slave_inject_packet 配套使用。
// 注意：out_len 至少要有 espnow_slave_packet_size() 字节。
void espnow_slave_build_test_packet(uint8_t *out, int out_len,
                                    uint16_t rpm, uint8_t speed, int16_t coolant,
                                    int16_t oil, int16_t intake, int16_t load,
                                    int16_t tps, int32_t bat_mv);

// 车况包的确切字节数。espnow_obd_packet_t 是 espnow_link.c 的私有类型，
// 外部拿不到 sizeof，所以在这里导出，避免调用方自己猜长度。
int espnow_slave_packet_size(void);

// 便捷入口：按给定数值构造并注入一个测试包（内部用正确长度）。
// 这是推荐的注入方式 —— 调用方不需要知道包有多大，也就不可能传错。
bool espnow_slave_inject_test_packet(uint16_t rpm, uint8_t speed, int16_t coolant,
                                     int16_t oil, int16_t intake, int16_t load,
                                     int16_t tps, int32_t bat_mv);

// 自检：注入一个已知数值的合成包，再读回车况缓存逐项比对并打印结论。
// 用途：只有一块板、没有主表时，证明「收包 → 解析 → 车况缓存」这段逻辑正确。
// 返回 true 表示全部字段往返一致。
// 由 ESPNOW_SLAVE_SELF_TEST（默认 1）在启动后自动调用一次。
bool espnow_slave_self_test(void);

#ifdef __cplusplus
}
#endif
