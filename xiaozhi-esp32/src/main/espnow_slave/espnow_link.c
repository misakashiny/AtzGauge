// espnow_link.c -- ESP-NOW 从表接收（从 obd_brz_gauge 移植）
//
// 上游：obd_brz_gauge/main/bsp_obd_dsp/espnow_link.c（515 行，含主表+从表）
// 这里只保留【从表】路径，并去掉了与仪表强耦合的部分。
//
// ── 三处必须理解的设计决定 ────────────────────────────────────────────────
//
// 1) 不初始化 WiFi，也不设信道。
//    上游 wifi_espnow_init() 会 esp_wifi_set_mode/start/set_channel，但小智固件的
//    WiFi 归 WifiBoard 管，且必须连上路由器才能上云。这里只 esp_now_init()。
//    信道方面：esp_now.h 明写 peer.channel == 0 表示「用 station 当前所在信道」，
//    ESP-NOW 走 STA 接口，因此自动跟随路由器信道。
//    ★ 硬约束：仪表主表把信道硬编码为 1（其 espnow_link.c 的 ESPNOW_CHANNEL），
//      所以路由器必须工作在【信道 1】，否则收不到任何包。这是 07 号文档讨论的
//      信道冲突，在这里以「主表不动、路由器迁就」的方式解决。
//
// 2) 不依赖 NVS。上游从表用 nvs_storage 存主表 MAC、读本机 position。
//    阶段 A 不做绑定（接受任何主表），position 固定 1。
//    绑定接口 espnow_slave_bind_master() 已留出，只作用于内存，后续接 Settings 即可。
//
// 3) presence（在线心跳）默认关闭。
//    上游从表每 500ms 广播一个 presence 包，主表据此统计在线从表数。但主表用
//    「在线从表数 > 0」来决定是否播放三连表开机动画——而本设备是小智板、没有仪表
//    位置，让主表误以为有仪表从表在线会引起不必要的联动。故默认不发。
//    若日后要让本板参与联动，把 ESPNOW_SLAVE_SEND_PRESENCE 改成 1。

#include "espnow_link.h"
#include "obd_data_cache.h"

#include <stdio.h>   // snprintf (build_test_packet)
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_idf_version.h"

static const char *TAG = "espnow_slave";

// ── 与主表逐字节兼容的协议常量（改这里必须同步改主表，否则收不到）──────────
#define ESPNOW_MAGIC            0x4F42  // 'OB'
#define ESPNOW_VER              5       // v5: 含 afr_x100
#define MASTER_NAME_LEN         12
#define ESPNOW_BROADCAST_CHANNEL 0      // 0 = 跟随 STA 当前信道（见文件头说明 1）

#define SLAVE_DATA_TIMEOUT_US   2000000 // 2s 内收到过即算在线
#define PRESENCE_INTERVAL_MS    500

// 默认不发 presence（见文件头说明 3）
#ifndef ESPNOW_SLAVE_SEND_PRESENCE
#define ESPNOW_SLAVE_SEND_PRESENCE 0
#endif

// 启动后自动跑一次自检（注入合成包验证「收包 → 缓存」）。
// 台架验证用，正式使用可设 0 关掉。
#ifndef ESPNOW_SLAVE_SELF_TEST
#define ESPNOW_SLAVE_SELF_TEST 1
#endif

#define ESPNOW_SLAVE_FIXED_POSITION 1   // 阶段 A：本板无仪表位置，固定 1

// ── 主表广播包结构（必须与 obd_brz_gauge 的 espnow_obd_packet_t 完全一致）──
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  flags;             // bit0: 主表已连 ELM327；bit1: 联动测试中
    uint32_t seq;               // 递增序号，用于丢包诊断
    uint16_t rpm;
    uint8_t  speed;
    uint8_t  sweep_step;
    uint8_t  intro_step;
    int16_t  coolant_temp;
    int16_t  intake_temp;
    int16_t  oil_temp;
    int16_t  oil_pressure_x10;
    int16_t  boost_x10;
    int16_t  brake_temp_x10;
    int16_t  load_pct;
    int16_t  tps;
    int32_t  bat_mv;
    int16_t  afr_x100;
    char     name[MASTER_NAME_LEN];
} espnow_obd_packet_t;

// presence 包（只在开启发送时需要）
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  position;
} espnow_presence_t;

static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static bool     s_started = false;
static volatile int64_t s_last_rx_us = 0;
static volatile uint32_t s_rx_count = 0;
static volatile uint32_t s_seq_last = 0;
static volatile uint32_t s_seq_gaps = 0;   // 累计丢包数（按序号跳变统计）
static char     s_master_name[MASTER_NAME_LEN] = {0};
static uint8_t  s_bound_master_mac[6] = {0};   // 全零 = 不限制

// ── 信道诊断状态 ────────────────────────────────────────────────────────────
// 为什么需要它：ESP-NOW 走 STA 接口、跟随 STA 信道，而主表固定在信道 1。
// 若 STA 连到别的信道的路由器，从表会「一个包都收不到」——这和「主表没开机 /
// 距离太远」在本模块里曾经完全无法区分，排查时只能靠猜。
// 这两种失败其实可以从 ESP-NOW 的角度区分开：信道不对时，射频偶尔仍会漏进来
// 几个本不属于我们的帧（其他设备的广播/组播）。把「长度/魔数不匹配」的帧计数
// 并连同最后来源 MAC 打出来，就能给出方向性证据：
//   mismatch 持续增长  → 射频是活的，但收到的是别人的流量 → 大概率信道不对
//   mismatch 恒为 0    → 这个信道上根本没有 ESP-NOW 流量 → 主表没开或太远
#define CHAN_WARN_INTERVAL_US (5 * 1000000LL)
static volatile uint32_t s_rx_mismatch = 0;        // 收到但解析失败的帧数
static volatile int64_t  s_last_mismatch_us = 0;
static volatile int64_t  s_last_chan_warn_us = 0;
static uint8_t  s_mismatch_mac[6] = {0};           // 最后一个解析失败帧的来源
static volatile int s_mismatch_len = 0;

// ── 收包核心 ───────────────────────────────────────────────────────────────
static void apply_packet(const espnow_obd_packet_t *p)
{
    obd_data_set_rpm(p->rpm);
    obd_data_set_speed(p->speed);
    obd_data_set_coolant_temp(p->coolant_temp);
    obd_data_set_intake_temp(p->intake_temp);
    obd_data_set_oil_temp(p->oil_temp);
    obd_data_set_oil_pressure_x10(p->oil_pressure_x10);
    obd_data_set_boost_x10(p->boost_x10);
    obd_data_set_brake_temp_x10(p->brake_temp_x10);
    obd_data_set_load_pct(p->load_pct);
    obd_data_set_tps(p->tps);
    obd_data_set_bat_mv(p->bat_mv);
    obd_data_set_afr_x100(p->afr_x100);

    memcpy(s_master_name, p->name, MASTER_NAME_LEN);
    s_master_name[MASTER_NAME_LEN - 1] = '\0';

    // 丢包诊断：主表 seq 每包 +1，跳变即为丢包
    if (s_seq_last != 0 && p->seq > s_seq_last + 1) {
        s_seq_gaps += (p->seq - s_seq_last - 1);
    }
    s_seq_last = p->seq;
}

static void handle_obd(const espnow_obd_packet_t *p)
{
    s_last_rx_us = esp_timer_get_time();
    uint32_t n = ++s_rx_count;
    apply_packet(p);

    // 10Hz 全打会淹没串口：首包立刻打，之后约 1Hz 打一次
    if (n == 1 || (n % 10) == 0) {
        char line[256];
        obd_data_format_status(line, sizeof(line));
        ESP_LOGI(TAG, "[#%u seq=%u gaps=%u] %s", (unsigned)n, (unsigned)p->seq,
                 (unsigned)s_seq_gaps, line);
    }
}

static bool mac_is_bound(void)
{
    for (int i = 0; i < 6; i++) {
        if (s_bound_master_mac[i] != 0) return true;
    }
    return false;
}

static void handle_rx(const uint8_t *mac, const uint8_t *data, int len)
{
    // 绑定后只接受该主表；未绑定时接受任何主表
    if (mac && mac_is_bound() && memcmp(mac, s_bound_master_mac, 6) != 0) {
        return;
    }

    // 记录「收到了帧但解析失败」——信道不对时的关键线索（见信道诊断状态注释）
    if (len != (int)sizeof(espnow_obd_packet_t)) {
        s_rx_mismatch++;
        s_last_mismatch_us = esp_timer_get_time();
        s_mismatch_len = len;
        if (mac) memcpy(s_mismatch_mac, mac, 6);
        else     memset(s_mismatch_mac, 0, 6);
        return;
    }

    // 长度虽然对上了，但魔数/版本不对，同样是别人的流量
    const espnow_obd_packet_t *p = (const espnow_obd_packet_t *)data;
    if (p->magic != ESPNOW_MAGIC || p->version != ESPNOW_VER) {
        s_rx_mismatch++;
        s_last_mismatch_us = esp_timer_get_time();
        s_mismatch_len = len;
        if (mac) memcpy(s_mismatch_mac, mac, 6);
        else     memset(s_mismatch_mac, 0, 6);
        ESP_LOGW(TAG, "OBD packet rejected: magic=0x%04X ver=%u (expect 0x%04X/%u) src=%02x:%02x:%02x:%02x:%02x:%02x",
                 p->magic, p->version, ESPNOW_MAGIC, ESPNOW_VER,
                 s_mismatch_mac[0], s_mismatch_mac[1], s_mismatch_mac[2],
                 s_mismatch_mac[3], s_mismatch_mac[4], s_mismatch_mac[5]);
        return;
    }

    handle_obd(p);
}

// IDF 5.0 起回调签名变化；IDF 6.x 用前者。保留条件编译以便日后回退。
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    handle_rx(info ? info->src_addr : NULL, data, len);
}
#else
static void recv_cb(const uint8_t *mac, const uint8_t *data, int len)
{
    handle_rx(mac, data, len);
}
#endif

#if ESPNOW_SLAVE_SEND_PRESENCE
static void presence_task(void *arg)
{
    espnow_presence_t pr = { .magic = ESPNOW_MAGIC, .version = ESPNOW_VER,
                             .position = ESPNOW_SLAVE_FIXED_POSITION };
    for (;;) {
        esp_now_send(s_broadcast_mac, (const uint8_t *)&pr, sizeof(pr));
        vTaskDelay(pdMS_TO_TICKS(PRESENCE_INTERVAL_MS));
    }
}
#endif

// ── 信道监视任务 ─────────────────────────────────────────────────────────────
// 周期检查「STA 信道 vs 主表信道(1)」，并把信道不对的证据主动打到串口。
// 不做任何自动切信道：STA 一旦连上 AP，射频信道由 AP 决定，强行 esp_wifi_set_channel()
// 会把云连接踢掉。所以这里只负责让故障「自己说出来」。
static void chan_monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        if (s_rx_count > 0) continue;   // 已经收到过主表数据，信道显然是对的

        uint8_t ch = 0;
        wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
        bool have_ch = (esp_wifi_get_channel(&ch, &sec) == ESP_OK);

        bool mismatch_active = (s_last_mismatch_us > 0) &&
                               ((esp_timer_get_time() - s_last_mismatch_us) < CHAN_WARN_INTERVAL_US);
        if (!mismatch_active) continue;

        // 有外来帧、却没有任何主表包 —— 最可能是信道不齐
        int64_t now = esp_timer_get_time();
        if ((now - s_last_chan_warn_us) < CHAN_WARN_INTERVAL_US) continue;
        s_last_chan_warn_us = now;

        ESP_LOGW(TAG, "no master packet yet, but %u foreign frame(s) arrived "
                      "(last: %d bytes from %02x:%02x:%02x:%02x:%02x:%02x)",
                 (unsigned)s_rx_mismatch, s_mismatch_len,
                 s_mismatch_mac[0], s_mismatch_mac[1], s_mismatch_mac[2],
                 s_mismatch_mac[3], s_mismatch_mac[4], s_mismatch_mac[5]);
        if (have_ch) {
            ESP_LOGW(TAG, "-> radio is alive on channel %u while the gauge master transmits on "
                          "channel 1. Set the router/hotspot to channel 1 (or move the master "
                          "to channel %u).", ch, ch);
        } else {
            ESP_LOGW(TAG, "-> the gauge master transmits on channel 1; the STA channel could not be read.");
        }
    }
}

// ── 对外接口 ───────────────────────────────────────────────────────────────
bool espnow_slave_start(void)
{
    if (s_started) {
        ESP_LOGI(TAG, "already started");
        return true;
    }

    // 注意：此处【不】调用 esp_wifi_init / set_mode / start / set_channel。
    // 小智的 WifiBoard 已经掌管 WiFi，重复初始化会破坏其状态。
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return false;
    }

    // 广播 peer：channel=0 → 跟随 STA 当前信道（见文件头说明 1）
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_broadcast_mac, 6);
    peer.channel = ESPNOW_BROADCAST_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "esp_now_add_peer(broadcast) failed: %s", esp_err_to_name(err));
    }

    err = esp_now_register_recv_cb(recv_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_register_recv_cb failed: %s", esp_err_to_name(err));
        return false;
    }

    s_started = true;

    uint8_t ch = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&ch, &sec) == ESP_OK) {
        ESP_LOGI(TAG, "ESP-NOW slave up; following STA channel %u (master broadcasts on ch1)", ch);
        if (ch != 1) {
            ESP_LOGW(TAG, "STA is on channel %u but the gauge master broadcasts on channel 1 "
                          "-> set your router/hotspot to channel 1 or no packets will arrive", ch);
        }
    } else {
        ESP_LOGW(TAG, "ESP-NOW slave up, but could not read the STA channel");
    }

#if ESPNOW_SLAVE_SEND_PRESENCE
    xTaskCreate(presence_task, "espnow_pres", 2560, NULL, 4, NULL);
    ESP_LOGI(TAG, "presence enabled (pos %d, every %dms)", ESPNOW_SLAVE_FIXED_POSITION, PRESENCE_INTERVAL_MS);
#else
    ESP_LOGI(TAG, "presence disabled (this board is a voice slave, not a gauge)");
#endif

#if ESPNOW_SLAVE_SELF_TEST
    // 台架证据：不依赖主表，证明「收包 → 解析 → 车况缓存」这段逻辑正确
    espnow_slave_self_test();
#endif

    // 信道监视：收不到包时主动给出「信道不对」还是「主表不在」的判据
    xTaskCreate(chan_monitor_task, "espnow_chan", 3072, NULL, 3, NULL);

    return true;
}

// 等 STA 真正连上 AP 再启动。原因：ESP-NOW 跟随 STA 信道，而 STA 未连接时
// （配网阶段是 APSTA、信道未定）启动会导致信道不对齐。
static void start_when_ready_task(void *arg)
{
    (void)arg;
    // 最长等 60s；期间每 500ms 探一次
    for (int i = 0; i < 120; i++) {
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "STA connected to \"%s\" on channel %u -> starting ESP-NOW",
                     (const char *)ap.ssid, ap.primary);
            espnow_slave_start();
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    // 没等到也启动：至少让注入式验证 / 日志可用，并留下明确线索
    ESP_LOGW(TAG, "STA not connected after 60s; starting ESP-NOW anyway");
    espnow_slave_start();
    vTaskDelete(NULL);
}

void espnow_slave_start_async(void)
{
    if (s_started) {
        ESP_LOGI(TAG, "already started, async start skipped");
        return;
    }
    xTaskCreate(start_when_ready_task, "espnow_wait", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "ESP-NOW slave will start once the STA is connected");
}

bool espnow_slave_has_data(void)
{
    int64_t last = s_last_rx_us;
    if (last == 0) return false;
    return (esp_timer_get_time() - last) < SLAVE_DATA_TIMEOUT_US;
}

int64_t espnow_slave_last_rx_age_ms(void)
{
    int64_t last = s_last_rx_us;
    if (last == 0) return -1;
    return (esp_timer_get_time() - last) / 1000;
}

uint32_t espnow_slave_rx_count(void) { return s_rx_count; }

uint32_t espnow_slave_rx_mismatch_count(void) { return s_rx_mismatch; }

const char *espnow_slave_master_name(void) { return s_master_name; }

void espnow_slave_bind_master(const uint8_t mac[6])
{
    if (!mac) return;
    memcpy(s_bound_master_mac, mac, 6);
    ESP_LOGI(TAG, "bound to master %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const uint8_t *espnow_slave_get_bound_master_mac(void) { return s_bound_master_mac; }

// ── 阶段 A 验证：不经过射频，直接把包喂进接收链路 ──────────────────────────
// 目的：在只有一块板（没有主表）的情况下，证明「收包 → 解析 → 车况缓存」这段
// 逻辑正确。射频那一段留到阶段 B 用第二块板验证。
bool espnow_slave_inject_packet(const uint8_t *data, int len)
{
    if (!data || len != (int)sizeof(espnow_obd_packet_t)) {
        ESP_LOGW(TAG, "inject: bad length %d (expect %d)", len, (int)sizeof(espnow_obd_packet_t));
        return false;
    }
    const espnow_obd_packet_t *p = (const espnow_obd_packet_t *)data;
    if (p->magic != ESPNOW_MAGIC || p->version != ESPNOW_VER) {
        ESP_LOGW(TAG, "inject: magic/version mismatch");
        return false;
    }
    // 用 NULL 源地址：注入路径不受 MAC 绑定限制
    handle_rx(NULL, data, len);
    return true;
}

void espnow_slave_build_test_packet(uint8_t *out, int out_len,
                                    uint16_t rpm, uint8_t speed, int16_t coolant,
                                    int16_t oil, int16_t intake, int16_t load,
                                    int16_t tps, int32_t bat_mv)
{
    if (!out || out_len < (int)sizeof(espnow_obd_packet_t)) return;
    espnow_obd_packet_t p;
    memset(&p, 0, sizeof(p));
    p.magic        = ESPNOW_MAGIC;
    p.version      = ESPNOW_VER;
    p.flags        = 0x01;          // bit0: 主表已连 OBD
    p.seq          = ++s_seq_last;  // 与真实路径共用序号逻辑
    p.rpm          = rpm;
    p.speed        = speed;
    p.coolant_temp = coolant;
    p.oil_temp     = oil;
    p.intake_temp  = intake;
    p.load_pct     = load;
    p.tps          = tps;
    p.bat_mv       = bat_mv;
    p.afr_x100     = -1;            // 阿特兹不支持
    p.boost_x10    = -32768;        // 自然吸气
    p.oil_pressure_x10 = -1;
    p.brake_temp_x10   = -1000;
    snprintf(p.name, MASTER_NAME_LEN, "SkyGauge");
    memcpy(out, &p, sizeof(p));
}

int espnow_slave_packet_size(void)
{
    return (int)sizeof(espnow_obd_packet_t);
}

bool espnow_slave_inject_test_packet(uint16_t rpm, uint8_t speed, int16_t coolant,
                                     int16_t oil, int16_t intake, int16_t load,
                                     int16_t tps, int32_t bat_mv)
{
    // 缓冲区大小由类型自身决定，调用方无从传错
    uint8_t buf[sizeof(espnow_obd_packet_t)];
    espnow_slave_build_test_packet(buf, (int)sizeof(buf), rpm, speed, coolant,
                                   oil, intake, load, tps, bat_mv);
    return espnow_slave_inject_packet(buf, (int)sizeof(buf));
}

// 自检：注入已知值 → 读回缓存 → 逐项比对。
// 注意结果用 ESP_LOGI/LOGE 打印，不依赖任何外部工具。
bool espnow_slave_self_test(void)
{
    const uint16_t t_rpm = 3210;
    const uint8_t  t_spd = 87;
    const int16_t  t_cool = 91;
    const int16_t  t_oil = 104;
    const int16_t  t_iat = 38;
    const int16_t  t_load = 42;
    const int16_t  t_tps = 27;
    const int32_t  t_bat = 14200;   // 14.20 V

    uint8_t buf[sizeof(espnow_obd_packet_t)];
    espnow_slave_build_test_packet(buf, sizeof(buf), t_rpm, t_spd, t_cool,
                                   t_oil, t_iat, t_load, t_tps, t_bat);

    if (!espnow_slave_inject_packet(buf, sizeof(buf))) {
        ESP_LOGE(TAG, "SELF-TEST FAIL: packet rejected by inject path");
        return false;
    }

    obd_data_snapshot_t s;
    obd_data_get_snapshot(&s);

    char line[256];
    obd_data_format_status(line, sizeof(line));
    ESP_LOGI(TAG, "SELF-TEST cache readback: %s", line);

    int bad = 0;
    struct { const char *name; long got, want; } checks[] = {
        { "rpm",          s.rpm,           t_rpm  },
        { "speed",        s.speed,         t_spd  },
        { "coolant_temp", s.coolant_temp,  t_cool },
        { "oil_temp",     s.oil_temp,      t_oil  },
        { "intake_temp",  s.intake_temp,   t_iat  },
        { "load_pct",     s.load_pct,      t_load },
        { "tps",          s.tps,           t_tps  },
        { "bat_mv",       s.bat_mv,        t_bat  },
    };
    for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        if (checks[i].got != checks[i].want) {
            ESP_LOGE(TAG, "  MISMATCH %-13s got=%ld want=%ld",
                     checks[i].name, checks[i].got, checks[i].want);
            bad++;
        }
    }

    if (bad == 0) {
        ESP_LOGI(TAG, "SELF-TEST PASS: packet -> cache path verified (%u fields)",
                 (unsigned)(sizeof(checks) / sizeof(checks[0])));
        ESP_LOGI(TAG, "  master name heard: \"%s\"", espnow_slave_master_name());
        return true;
    }
    ESP_LOGE(TAG, "SELF-TEST FAIL: %d field(s) mismatched", bad);
    return false;
}
