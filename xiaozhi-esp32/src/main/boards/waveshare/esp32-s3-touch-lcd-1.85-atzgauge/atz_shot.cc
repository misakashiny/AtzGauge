// atz_shot.cc -- 本地截图端点实现（见 atz_shot.h 的说明）

#include "atz_shot.h"
#include "atz_ui_config.h"

#include "display.h"
#include "espnow_link.h"
#include "atz_car_page.h"
#include "atz_hit_zone.h"
#include "atz_perf.h"
#include "atz_rpm_ring.h"
#include "atz_trip.h"
#include "atz_touch.h"
#include "atz_ui.h"
#include "jpg/image_to_jpeg.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#define TAG "AtzShot"

namespace {
Display* g_display = nullptr;
httpd_handle_t g_server = nullptr;

// 前置声明：StartTask 里要注册这两个 handler，而它们的定义在文件后面
esp_err_t CarBarHandler(httpd_req_t* req);
esp_err_t TouchHandler(httpd_req_t* req);
esp_err_t TapHandler(httpd_req_t* req);
esp_err_t SwipeHandler(httpd_req_t* req);
esp_err_t AssetsHandler(httpd_req_t* req);
esp_err_t CarPageHandler(httpd_req_t* req);
esp_err_t SizeHandler(httpd_req_t* req);
esp_err_t PerfHandler(httpd_req_t* req);
esp_err_t LayoutHandler(httpd_req_t* req);
esp_err_t SubtitleHandler(httpd_req_t* req);
esp_err_t InjectHandler(httpd_req_t* req);
esp_err_t RingHandler(httpd_req_t* req);
esp_err_t TripHandler(httpd_req_t* req);

// ── 调试端点鉴权 ────────────────────────────────────────────────────────────
// 只给"写操作"加锁：/theme /carbar /carpage /size /inject /ring /subtitle。
// 只读端点（/health /shot.jpg /layout /perf /touch）不校验，方便随时探活。
// 这不是强安全措施（token 是明文常量、串口日志里也有），只是挡住"同网段随手乱改"。
bool WriteAllowed(httpd_req_t* req) {
#if ATZ_DEBUG_AUTH
    char query[256] = {};
    char key[64] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(query, "key", key, sizeof(key)) != ESP_OK) {
        return false;
    }
    return strcmp(key, ATZ_DEBUG_TOKEN) == 0;
#else
    (void)req;
    return true;
#endif
}

/** 鉴权失败时统一回一句话（并告诉调用方别再往下走）。 */
esp_err_t DenyWrite(httpd_req_t* req) {
    return httpd_resp_send(req,
                           "denied: write endpoints need &key=<ATZ_DEBUG_TOKEN>\n"
                           "(token is printed in the device boot log; see atz_ui_config.h)\n",
                           HTTPD_RESP_USE_STRLEN);
}

// ── /perf 的非阻塞采样器 ────────────────────────────────────────────────────
// ★ 以前 /perf 是"在 HTTP 回调里 vTaskDelay(N 秒) 再统计" —— 那会把 1/2 个 socket
//   占死 N 秒，别的调试请求全被拒。现在改成后台任务持续采样 2 秒窗口，
//   端点只把**上一次算好的结果**立刻返回（带窗口与新鲜度）。
struct PerfSample {
    uint32_t fps_x10;
    uint32_t avg_us;
    uint32_t avg_px;
    uint32_t peak_px;
    uint32_t calls;        // 转速环定时器调用次数（环关掉时为 0）
    uint32_t pushes;       // 转速环真正改控件的次数
    uint32_t ui_pushes;    // 其它 UI 模块上报的改控件次数（atz_perf_count_push）
    uint32_t frames;
    int64_t at_ms;
};
PerfSample g_perf = {};
bool g_perf_task_started = false;

void PerfSamplerTask(void* arg) {
    (void)arg;
    const int window_ms = 2000;
    for (;;) {
        uint32_t f0 = 0, c0 = 0, p0 = 0, im0 = 0, pc0 = 0;
        uint64_t b0 = 0, px0 = 0, w0 = 0;
        atz_perf_read(&f0, &b0, &pc0);            // 显示层：帧数 / 渲染耗时（atz_perf.cc）
        atz_perf_ext(&w0, &px0, &im0);            // 显示层：最慢帧 / 重绘像素
        atz_rpm_ring_perf_read(&c0, &p0);         // 转速环自己的计数（环关掉时为 0）

        vTaskDelay(pdMS_TO_TICKS(window_ms));

        uint32_t f1 = 0, c1 = 0, p1 = 0, im1 = 0, pc1 = 0;
        uint64_t b1 = 0, px1 = 0, w1 = 0;
        atz_perf_read(&f1, &b1, &pc1);
        atz_perf_ext(&w1, &px1, &im1);
        atz_rpm_ring_perf_read(&c1, &p1);
        const uint32_t frames = f1 - f0;
        const uint64_t busy = b1 - b0;
        const uint64_t px = px1 - px0;
        g_perf.frames = frames;
        // fps_x10 = 帧数 / 窗口秒数 * 10；窗口 2000ms 时 = frames * 10 / 2
        g_perf.fps_x10 = (uint32_t)((uint64_t)frames * 10000 / window_ms);
        g_perf.avg_us = frames ? (uint32_t)(busy / frames) : 0;
        g_perf.avg_px = frames ? (uint32_t)(px / frames) : 0;
        g_perf.peak_px = im1;
        g_perf.calls = c1 - c0;
        g_perf.pushes = p1 - p0;
        g_perf.ui_pushes = pc1 - pc0;
        g_perf.at_ms = esp_timer_get_time() / 1000;
    }
}

void PerfSamplerStart(void) {
    if (g_perf_task_started) {
        return;
    }
    g_perf_task_started = true;
    xTaskCreate(PerfSamplerTask, "atz_perf", 4096, nullptr, 2, nullptr);
}
// 递归 dump 控件树（/layout 用）。class 用指针比对，避免依赖 LVGL 内部类的字段。
void DumpObjTree(lv_obj_t* obj, int depth, char* out, size_t len, size_t* used) {
    if (obj == nullptr || *used + 96 >= len) {
        return;
    }
    const lv_obj_class_t* cls = lv_obj_get_class(obj);
    const char* kind = "obj";
    if (cls == &lv_label_class) {
        kind = "label";
    } else if (cls == &lv_image_class) {
        kind = "image";
    } else if (cls == &lv_arc_class) {
        kind = "arc";
    } else if (cls == &lv_button_class) {
        kind = "btn";
    }

    lv_area_t a = {0, 0, 0, 0};
    lv_obj_get_coords(obj, &a);
    const bool hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    int n = snprintf(out + *used, len - *used, "%*s%-5s (%3d,%3d)-(%3d,%3d) %3dx%-3d%s",
                     depth * 2, "", kind, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2,
                     (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1), hidden ? " HIDE" : "     ");
    if (n > 0) {
        *used += (size_t)n;
    }
    if (cls == &lv_label_class && *used + 48 < len) {
        const char* t = lv_label_get_text(obj);
        if (t != nullptr && t[0] != '\0') {
            n = snprintf(out + *used, len - *used, " \"%.24s\"", t);
            if (n > 0) {
                *used += (size_t)n;
            }
        }
    }
    if (*used + 2 < len) {
        out[(*used)++] = '\n';
        out[*used] = '\0';
    }

    const uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; i++) {
        DumpObjTree(lv_obj_get_child(obj, (int32_t)i), depth + 1, out, len, used);
        if (*used + 96 >= len) {
            return;
        }
    }
}

/**
 * 自己实现一份截图，不用 Display::SnapshotToJpeg()。
 *
 * ★ 为什么不直接用上游那个：LvglDisplay::SnapshotToJpeg() 对 LVGL 的 RGB565 数据做了
 *   __builtin_bswap16()，再把格式声明成 V4L2_PIX_FMT_RGB565。而编码器源码写得很清楚：
 *       image_to_jpeg.cpp:137  V4L2_PIX_FMT_RGB565 -> ESP_IMGFX_PIXEL_FMT_RGB565_LE
 *       image_to_jpeg.cpp:245  该分支只做 memcpy，不做任何字节交换
 *   （只有 YUYV 分支需要 bswap，见 image_to_jpeg.cpp:259 的注释。）
 *   LVGL 在 ESP32 小端上存的本来就是 LE，所以上游那次 bswap 属于多余且错误 ——
 *   实测背景 #F2F4F7 被拍成 (184,220,184)，颜色完全错乱。
 *   附带影响：MCP 工具 self.screen.snapshot 上传给大模型看的图同样是错色的。
 *   这里按编码器的真实约定直接送数据，颜色才可信。
 */
bool TakeSnapshotJpeg(std::string& jpeg_out) {
    if (g_display == nullptr) {
        return false;
    }
    DisplayLockGuard lock(g_display);
    lv_draw_buf_t* buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (buf == nullptr) {
        ESP_LOGE(TAG, "lv_snapshot_take failed");
        return false;
    }
    jpeg_out.clear();
    bool ok = image_to_jpeg_cb((uint8_t*)buf->data, buf->data_size, buf->header.w, buf->header.h,
                               V4L2_PIX_FMT_RGB565, ATZ_UI_SHOT_JPEG_QUALITY,
                               [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                                   (void)index;
                                   if (data != nullptr && len > 0) {
                                       static_cast<std::string*>(arg)->append(
                                           static_cast<const char*>(data), len);
                                   }
                                   return len;
                               },
                               &jpeg_out);
    lv_draw_buf_destroy(buf);
    if (!ok) {
        ESP_LOGE(TAG, "image_to_jpeg_cb failed");
    }
    return ok;
}

esp_err_t ShotHandler(httpd_req_t* req) {
    if (g_display == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no display");
        return ESP_FAIL;
    }

    std::string jpeg;
    if (!TakeSnapshotJpeg(jpeg)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, jpeg.data(), jpeg.size());
    ESP_LOGI(TAG, "served snapshot: %u bytes", (unsigned)jpeg.size());
    return err;
}

// 探活 + **版本探针**：第一行仍是 "ok"（脚本里原有的判断不受影响），
// 后面跟固件标识与关键开关 —— 以后不用再猜"设备上跑的是哪一版"。
esp_err_t HealthHandler(httpd_req_t* req) {
    char body[320];
    snprintf(body, sizeof(body),
             "ok\n"
             "fw=%s %s\n"
             "built=%s\n"
             "switches: ATZ_UI_ENABLE=%d rpm_ring=%d arc_text=%d wifi_icon=%d\n"
             "uptime=%lu s  heap=%lu B\n",
             ATZ_FW_NAME, ATZ_FW_STAGE, ATZ_FW_BUILT, ATZ_UI_ENABLE, ATZ_RPM_RING_ENABLE,
             ATZ_ARC_TEXT_ENABLE, ATZ_SHOW_NETWORK_ICON,
             (unsigned long)(esp_timer_get_time() / 1000000),
             (unsigned long)esp_get_free_heap_size());
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// 台架用：从 PC 直接切主题，不必对着设备说话。
//   http://<设备IP>:8099/theme?name=atz-day
// 主题名会被写进 NVS（Display::SetTheme 的行为），所以和语音切换等价。
esp_err_t ThemeHandler(httpd_req_t* req) {
    if (!WriteAllowed(req)) {   // 写操作需要 &key=<ATZ_DEBUG_TOKEN>
        return DenyWrite(req);
    }
    if (g_display == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no display");
        return ESP_FAIL;
    }
    char query[64] = {};
    char name[32] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "usage: /theme?name=atz-night");
        return ESP_FAIL;
    }
    auto* theme = LvglThemeManager::GetInstance().GetTheme(name);
    if (theme == nullptr) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown theme");
        return ESP_FAIL;
    }
    g_display->SetTheme(theme);   // 内部会加显示锁并写入 NVS
    ESP_LOGI(TAG, "theme set to %s via http", name);
    char body[48];
    snprintf(body, sizeof(body), "ok: %s", name);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// WiFi 拿到 IP 之后再启动 HTTP 服务。
//
// ★ 为什么不能直接起：板级构造函数跑在 Application::Initialize() 之前，那时 lwIP 还没
//   初始化完毕。此时 socket()/bind() 会走到 tcpip_send_msg_wait_sem()，而 tcpip 线程的
//   mbox 仍是 NULL → assert failed: tcpip_send_msg_wait_sem (Invalid mbox) → 无限重启。
//   （这个坑已经踩过一次：实测 30 秒重启 9 次。）
//   所以先等 STA 网卡出现且拿到 IP，再 httpd_start()。
void StartTask(void* arg) {
    (void)arg;
    for (int i = 0; i < 240; i++) {   // 最多等 120 秒
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip = {};
        if (netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            httpd_config_t config = HTTPD_DEFAULT_CONFIG();
            config.server_port = ATZ_UI_SHOT_SERVER_PORT;
            // ★ ctrl_port 必须与其它 httpd 实例不同（默认 32768 已被 OTA/配网服务器占用），
            //   否则 httpd_start() 会失败。
            config.ctrl_port = 32770;
            config.stack_size = 6144;   // 快照 + JPEG 编码的调用栈
            config.lru_purge_enable = true;
            // ★★ socket 预算（2026-09-16 血的教训）★★
            //   本机 lwIP 总共只有 CONFIG_LWIP_MAX_SOCKETS=10 个 socket，而它们要和
            //   **与云端的 WebSocket/MQTT 连接**共用。PC 端模拟台按 10Hz 打 /inject 时，
            //   httpd 默认要 7 个 socket，直接把这个池子吃光 → 串口报
            //       E httpd: httpd_accept_conn: error in accept (23)      ← errno 23 = ENFILE
            //   连云端连接都建不起来 → **喊"小智"没反应**（看着像"网络冲突"，其实是 socket 耗尽）。
            //   所以调试用的这个服务只准占 2 个，永远给云连接留足。
            config.max_open_sockets = 3;   // 3 个够用（快照/查询/注入各一），仍给云连接留足
            config.keep_alive_enable = true;      // 复用连接，减少握手（也就少占 socket）
            config.recv_wait_timeout = 5;
            config.send_wait_timeout = 5;
            // ★ 必须 ≥ 实际注册的端点个数：httpd_register_uri_handler 超限时只返回
            //   ESP_ERR_HTTPD_HANDLERS_FULL，**不会**报错也不会崩，表现为该 URL 直接 404。
            //   （踩过一次：设 4 而注册了 5 个 → /touch 一直 404。）
            config.max_uri_handlers = 16;   // 注意：加端点要同步改这里，否则注册失败只报一条 E 日志

            if (httpd_start(&g_server, &config) != ESP_OK) {
                ESP_LOGE(TAG, "failed to start snapshot server on port %d",
                         ATZ_UI_SHOT_SERVER_PORT);
                g_server = nullptr;
                vTaskDelete(nullptr);
                return;
            }

            httpd_uri_t shot_uri = {
                .uri = "/shot.jpg",
                .method = HTTP_GET,
                .handler = ShotHandler,
                .user_ctx = nullptr,
            };
            httpd_uri_t health_uri = {
                .uri = "/health",
                .method = HTTP_GET,
                .handler = HealthHandler,
                .user_ctx = nullptr,
            };
            httpd_uri_t theme_uri = {
                .uri = "/theme",
                .method = HTTP_GET,
                .handler = ThemeHandler,
                .user_ctx = nullptr,
            };
            httpd_uri_t carbar_uri = {
                .uri = "/carbar",
                .method = HTTP_GET,
                .handler = CarBarHandler,
                .user_ctx = nullptr,
            };
            httpd_uri_t touch_uri = {
                .uri = "/touch",
                .method = HTTP_GET,
                .handler = TouchHandler,
                .user_ctx = nullptr,
            };
            httpd_uri_t carpage_uri = {
                .uri = "/carpage",
                .method = HTTP_GET,
                .handler = CarPageHandler,
                .user_ctx = nullptr,
            };
            httpd_uri_t size_uri = {
                .uri = "/size",
                .method = HTTP_GET,
                .handler = SizeHandler,
                .user_ctx = nullptr,
            };
            httpd_uri_t perf_uri = {
                .uri = "/perf",
                .method = HTTP_GET,
                .handler = PerfHandler,
                .user_ctx = nullptr,
            };
            httpd_uri_t layout_uri = {
                .uri = "/layout",
                .method = HTTP_GET,
                .handler = LayoutHandler,
                .user_ctx = nullptr,
            };
            httpd_uri_t trip_uri = {
                .uri = "/trip",
                .method = HTTP_GET,
                .handler = TripHandler,
                .user_ctx = nullptr,
            };
            httpd_uri_t ring_uri = {
                .uri = "/ring",
                .method = HTTP_GET,
                .handler = RingHandler,
                .user_ctx = nullptr,
            };
            httpd_uri_t inject_uri = {
                .uri = "/inject",
                .method = HTTP_GET,
                .handler = InjectHandler,
                .user_ctx = nullptr,
            };
            httpd_uri_t subtitle_uri = {
                .uri = "/subtitle",
                .method = HTTP_GET,
                .handler = SubtitleHandler,
                .user_ctx = nullptr,
            };
            // /tap：合成一次点击（不碰触摸硬件），用来验证"按钮回调真的通了"
            httpd_uri_t tap_uri = {
                .uri = "/tap",
                .method = HTTP_GET,
                .handler = TapHandler,
                .user_ctx = nullptr,
            };
            httpd_uri_t swipe_uri = {
                .uri = "/swipe",
                .method = HTTP_GET,
                .handler = SwipeHandler,
                .user_ctx = nullptr,
            };
            httpd_uri_t assets_uri = {
                .uri = "/assets",
                .method = HTTP_GET,
                .handler = AssetsHandler,
                .user_ctx = nullptr,
            };
            httpd_register_uri_handler(g_server, &shot_uri);
            httpd_register_uri_handler(g_server, &health_uri);
            httpd_register_uri_handler(g_server, &theme_uri);
            httpd_register_uri_handler(g_server, &carbar_uri);
            if (httpd_register_uri_handler(g_server, &touch_uri) != ESP_OK) {
                ESP_LOGE(TAG, "/touch registration failed (max_uri_handlers too small?)");
            }
            if (httpd_register_uri_handler(g_server, &carpage_uri) != ESP_OK) {
                ESP_LOGE(TAG, "/carpage registration failed (max_uri_handlers too small?)");
            }
            if (httpd_register_uri_handler(g_server, &size_uri) != ESP_OK) {
                ESP_LOGE(TAG, "/size registration failed (max_uri_handlers too small?)");
            }
            if (httpd_register_uri_handler(g_server, &perf_uri) != ESP_OK) {
                ESP_LOGE(TAG, "/perf registration failed (max_uri_handlers too small?)");
            }
            if (httpd_register_uri_handler(g_server, &layout_uri) != ESP_OK) {
                ESP_LOGE(TAG, "/layout registration failed (max_uri_handlers too small?)");
            }
            if (httpd_register_uri_handler(g_server, &trip_uri) != ESP_OK) {
                ESP_LOGE(TAG, "/trip registration failed (max_uri_handlers too small?)");
            }
            if (httpd_register_uri_handler(g_server, &ring_uri) != ESP_OK) {
                ESP_LOGE(TAG, "/ring registration failed (max_uri_handlers too small?)");
            }
            if (httpd_register_uri_handler(g_server, &inject_uri) != ESP_OK) {
                ESP_LOGE(TAG, "/inject registration failed (max_uri_handlers too small?)");
            }
            if (httpd_register_uri_handler(g_server, &subtitle_uri) != ESP_OK) {
                ESP_LOGE(TAG, "/subtitle registration failed (max_uri_handlers too small?)");
            }
            if (httpd_register_uri_handler(g_server, &tap_uri) != ESP_OK) {
                ESP_LOGE(TAG, "/tap registration failed (max_uri_handlers too small?)");
            }
            if (httpd_register_uri_handler(g_server, &swipe_uri) != ESP_OK) {
                ESP_LOGE(TAG, "/swipe registration failed (max_uri_handlers too small?)");
            }
            if (httpd_register_uri_handler(g_server, &assets_uri) != ESP_OK) {
                ESP_LOGE(TAG, "/assets registration failed (max_uri_handlers too small?)");
            }

            ESP_LOGI(TAG, "screen snapshot ready: http://" IPSTR ":%d/shot.jpg", IP2STR(&ip.ip),
                     ATZ_UI_SHOT_SERVER_PORT);
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "no station IP after 120s; snapshot server not started");
    vTaskDelete(nullptr);
}

// 台架用：从 PC 注入合成车况（等价于语音工具 self.ui.car_bar_demo）
//   http://<设备IP>:8099/carbar?seconds=15               固定值（转速 3200 上下小幅摆动）
//   http://<设备IP>:8099/carbar?seconds=30&mode=rev      转速扫掠：怠速→拉转速→换挡→再拉→回怠速
//   http://<设备IP>:8099/carbar?seconds=30&mode=redline  同上但峰值 7000 转（越过红线，会出声告警）
esp_err_t CarBarHandler(httpd_req_t* req) {
    if (!WriteAllowed(req)) {   // 写操作需要 &key=<ATZ_DEBUG_TOKEN>
        return DenyWrite(req);
    }
    char query[96] = {};
    char value[16] = {};
    int seconds = 15;
    const char* mode = "demo";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "seconds", value, sizeof(value)) == ESP_OK) {
            seconds = atoi(value);
        }
        if (httpd_query_key_value(query, "mode", value, sizeof(value)) == ESP_OK) {
            mode = value;
        }
    }
    if (strcmp(mode, "rev") == 0 || strcmp(mode, "sweep") == 0) {
        atz_ui_car_rev_sim(seconds, false);
    } else if (strcmp(mode, "redline") == 0) {
        atz_ui_car_rev_sim(seconds, true);
    } else if (strcmp(mode, "stop") == 0 || strcmp(mode, "off") == 0 || seconds == 0) {
        atz_ui_car_inject_stop();   // ★ 停车：立刻结束正在跑的模拟，环会在 2 秒内归零
        mode = "stop";
    } else {
        atz_ui_car_bar_demo(seconds);
    }
    char body[96];
    snprintf(body, sizeof(body), "ok: car data inject %d s (mode=%s)", seconds, mode);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// 台架用：触摸面板自检 —— 打开页面后点屏幕，页面直接告诉你面板到底答不答。
//   http://<设备IP>:8099/touch?seconds=15
// 这是区分"固件逻辑问题"与"面板不应答 I2C"的唯一可靠手段。
// ★ 2026-09-17 追加：把**已注册的点击区**一起列出来 —— 按钮"点不动"时，先确认
//   "区到底注册上没有、坐标对不对"，再看面板答不答。
esp_err_t TouchHandler(httpd_req_t* req) {
    char query[64] = {};
    char value[16] = {};
    int seconds = 5;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "seconds", value, sizeof(value)) == ESP_OK) {
        seconds = atoi(value);
    }
    char report[512] = {};
    atz_touch_watch(seconds, report, sizeof(report));   // 阻塞 seconds 秒（调试端点，可接受）
    char body[768];
    int n = snprintf(body, sizeof(body), "%s\nhit zones (%d):", report, atz_hit_zone_count());
    for (int i = 0; i < atz_hit_zone_count() && n < (int)sizeof(body) - 48; i++) {
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        const char* name = nullptr;
        if (atz_hit_zone_info(i, &x1, &y1, &x2, &y2, &name)) {
            n += snprintf(body + n, sizeof(body) - n, "\n  [%d] %s (%d,%d)-(%d,%d)", i, name, x1,
                          y1, x2, y2);
        }
    }
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// 台架用：**合成一次点击**（不碰触摸硬件）—— 用来验证"命中区 → 按钮回调 → UI 刷新"
// 这条链路本身是通的，从而把"固件逻辑问题"和"面板/触摸读不到坐标"彻底分开。
//   http://<设备IP>:8099/tap?x=240&y=50&key=…     行程页「清零重来」按钮中心
//   http://<设备IP>:8099/tap?x=120&y=50&key=…     行程页「暂停/开始记录」按钮中心
//   http://<设备IP>:8099/tap?x=180&y=340&key=…    底部翻页提示条
// 不带 x/y 时返回**所有点击区的中心点**，方便直接照着点。
esp_err_t TapHandler(httpd_req_t* req) {
    char query[96] = {};
    char value[16] = {};
    int x = -1, y = -1;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "x", value, sizeof(value)) == ESP_OK) {
            x = atoi(value);
        }
        if (httpd_query_key_value(query, "y", value, sizeof(value)) == ESP_OK) {
            y = atoi(value);
        }
    }
    char body[768];
    if (x < 0 || y < 0) {
        int n = snprintf(body, sizeof(body), "hit zones = %d; center points to tap:", atz_hit_zone_count());
        for (int i = 0; i < atz_hit_zone_count() && n < (int)sizeof(body) - 56; i++) {
            int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            const char* name = nullptr;
            if (atz_hit_zone_info(i, &x1, &y1, &x2, &y2, &name)) {
                n += snprintf(body + n, sizeof(body) - n, "\n  [%d] %s -> /tap?x=%d&y=%d&key=…", i,
                              name, (x1 + x2) / 2, (y1 + y2) / 2);
            }
        }
        return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    }
    if (!WriteAllowed(req)) {   // 合成点击会改状态，算写操作
        return DenyWrite(req);
    }
    // 特殊：/tap?x=-1&y=-1 之外，用 /swipe 端点模拟滑动（见下）
    const int id = atz_hit_zone_test(x, y);
    const char* name = "(none)";
    if (id >= 0) {
        atz_hit_zone_info(id, nullptr, nullptr, nullptr, nullptr, &name);
        atz_hit_zone_fire(id);
        ESP_LOGI(TAG, "/tap (%d,%d) -> zone %d [%s] fired", x, y, id, name);
    } else {
        ESP_LOGI(TAG, "/tap (%d,%d) -> no zone hit", x, y);
    }
    snprintf(body, sizeof(body), "tap (%d,%d) -> zone=%d [%s]\n", x, y, id, name);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// 台架用：**换表情包/素材**（两条路，等价于语音工具 self.assets.set_download_url）
//   http://<IP>:8099/assets?url=http://192.168.5.128:8124/assets.bin&key=…&reboot=1
//     → 写 NVS(assets/download_url) 并（可选）重启；设备**开机时**会去下载并 apply。
//       这是上游支持的运行时换素材机制，不用重刷固件。配套工具：tools\emoji-kit.mjs
//   http://<IP>:8099/assets                                              → 只读：当前地址 + 上次进度
//   http://<IP>:8099/assets?clear=1&key=…                                → 清掉待下载地址（取消）
//   http://<IP>:8099/assets?progress=42&key=…                            → 由 PC 端工具上报进度（显示在字幕上）
// 为什么要这个端点（而不是直接调 MCP）：本板固件里 MCP 工具只走云端 WebSocket，
// 没有 HTTP 侧的调用入口 —— 而"换素材"必须能在**没有云连接**的台架上跑通。
esp_err_t AssetsHandler(httpd_req_t* req) {
    char query[256] = {};
    char value[192] = {};
    const bool has_query = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK;
    const bool want_url = has_query &&
                          httpd_query_key_value(query, "url", value, sizeof(value)) == ESP_OK;
    char prog_buf[16] = {};
    const bool want_prog = has_query &&
                           httpd_query_key_value(query, "progress", prog_buf, sizeof(prog_buf)) == ESP_OK;
    const bool want_clear = has_query &&
                            httpd_query_key_value(query, "clear", value, sizeof(value)) == ESP_OK;

    if (want_url || want_clear || want_prog) {
        if (!WriteAllowed(req)) {
            return DenyWrite(req);
        }
    }

    if (want_clear) {
        Settings("assets", true).EraseKey("download_url");
        char body[96];
        snprintf(body, sizeof(body), "pending assets url cleared\n");
        return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    }

    if (want_prog) {
        // 进度只显示在字幕上（不写 NVS）：PC 工具边收边报，用户能看见百分比
        char msg[32];
        snprintf(msg, sizeof(msg), "素材 %d%%", atoi(prog_buf));
        if (g_display != nullptr) {
            g_display->SetChatMessage("system", msg);
        }
        return httpd_resp_send(req, "ok\n", HTTPD_RESP_USE_STRLEN);
    }

    if (want_url) {
        if (strncmp(value, "http://", 7) != 0 && strncmp(value, "https://", 8) != 0) {
            char body[128];
            snprintf(body, sizeof(body), "denied: url must start with http:// or https://\n");
            return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
        }
        Settings("assets", true).SetString("download_url", value);
        ESP_LOGI(TAG, "/assets: pending download url set to %s", value);
        char do_reboot[8] = {};
        const bool reboot = has_query &&
                            httpd_query_key_value(query, "reboot", do_reboot, sizeof(do_reboot)) == ESP_OK &&
                            atoi(do_reboot) != 0;
        char body[384];
        snprintf(body, sizeof(body),
                 "ok: assets download url saved\n  %s\n  %s\n",
                 value,
                 reboot ? "rebooting now (download happens at boot)" :
                          "takes effect on the NEXT BOOT (send reboot=1 or power-cycle)");
        httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
        if (reboot) {
            vTaskDelay(pdMS_TO_TICKS(600));   // 让响应先发出去
            esp_restart();
        }
        return ESP_OK;
    }

    // 只读查询
    Settings s("assets", false);
    std::string pending = s.GetString("download_url", "");
    char body[320];
    snprintf(body, sizeof(body),
             "pending download url: %s\n"
             "note: the device downloads it at BOOT, then wipes the setting\n"
             "push:  /assets?url=http://<PC>:8124/assets.bin&reboot=1&key=...\n"
             "clear: /assets?clear=1&key=...\n",
             pending.empty() ? "(none - using built-in assets)" : pending.c_str());
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// 台架用：**合成一次滑动手势**（不碰触摸硬件）—— 验证"滑动 → 翻页/进车况页"这条链路。
//   http://<设备IP>:8099/swipe?dir=left&key=…    左划（车况页→行程页；主界面→车况页）
//   http://<设备IP>:8099/swipe?dir=right&key=…   右划（行程页→车况页）
// 不带 dir 时只报告当前注册了哪些滑动回调。
esp_err_t SwipeHandler(httpd_req_t* req) {
    char query[64] = {};
    char value[16] = {};
    const bool has = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                     httpd_query_key_value(query, "dir", value, sizeof(value)) == ESP_OK;
    char body[256];
    if (!has) {
        snprintf(body, sizeof(body),
                 "car page visible=%d page=%d\n"
                 "swipe handlers: LEFT=%d RIGHT=%d (0 = falls back to main-screen rule)\n"
                 "note: dir=left on the main screen is handled by the touch task\n",
                 (int)atz_car_page_visible(), atz_car_page_page(),
                 (int)atz_swipe_has_handler(ATZ_SWIPE_LEFT),
                 (int)atz_swipe_has_handler(ATZ_SWIPE_RIGHT));
        return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    }
    if (!WriteAllowed(req)) {
        return DenyWrite(req);
    }
    const int dir = (strcmp(value, "left") == 0) ? ATZ_SWIPE_LEFT : ATZ_SWIPE_RIGHT;
    if (!atz_swipe_has_handler(dir)) {
        // 没有注册回调 = 当前页不响应这个方向。主界面左划那条规则在触摸任务里，
        // 这里补一次等价动作，方便台架验证"主界面左划进车况页"。
        if (dir == ATZ_SWIPE_LEFT && !atz_car_page_visible()) {
            atz_car_page_show();
            snprintf(body, sizeof(body), "swipe LEFT (no handler) -> main-screen rule: car page shown\n");
            return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
        }
        snprintf(body, sizeof(body), "swipe %s -> no handler on this page (ignored)\n", value);
        return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    }
    atz_swipe_fire(dir);
    char rep[48] = {};
    atz_gesture_report(rep, sizeof(rep));
    snprintf(body, sizeof(body), "swipe %s fired -> page=%d  report=%s\n", value,
             atz_car_page_page(), rep);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// 台架用：显示/隐藏「车况」整屏页面（等价于语音工具 self.ui.show_car_page）
//   http://<设备IP>:8099/carpage?action=show | hide | toggle
//   http://<设备IP>:8099/carpage?page=1&key=…        翻到第 2 页（行程统计）
//   http://<设备IP>:8099/carpage?fields=rpm,coolant   只显示这几项（all = 全部）
//   http://<设备IP>:8099/carpage?on=intake&off=load   单项开关（等价于语音 set_car_page_field）
// ★ 不带任何参数的 GET 是**只读查询**（返回当前页/可见字段），不要求 token —— 与其他
//   只读端点（/health /perf /layout）保持一致；以前这里无条件要 key，探活时很别扭。
esp_err_t CarPageHandler(httpd_req_t* req) {
    char query[192] = {};
    char value[128] = {};
    const bool has_query = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK;
    const char* action = "show";
    if (has_query && httpd_query_key_value(query, "action", value, sizeof(value)) == ESP_OK) {
        action = value;
    }
    char page_val[16] = {};
    const bool want_page = has_query &&
                           httpd_query_key_value(query, "page", page_val, sizeof(page_val)) == ESP_OK;
    bool want_on = false, want_off = false, want_fields = false;
    if (has_query) {
        want_on = httpd_query_key_value(query, "on", value, sizeof(value)) == ESP_OK;
        want_off = httpd_query_key_value(query, "off", value, sizeof(value)) == ESP_OK;
        want_fields = httpd_query_key_value(query, "fields", value, sizeof(value)) == ESP_OK;
    }
    const bool is_write = (strcmp(action, "hide") == 0) || (strcmp(action, "toggle") == 0) ||
                          want_page || want_on || want_off || want_fields;
    if (is_write && !WriteAllowed(req)) {
        return DenyWrite(req);
    }

    if (strcmp(action, "hide") == 0) {
        atz_car_page_hide();
    } else if (strcmp(action, "toggle") == 0) {
        atz_car_page_toggle();
    } else if (is_write) {
        atz_car_page_show();
    }
    if (want_page) {
        atz_car_page_set_page(atoi(page_val));
    }

    char body[320];
    int written = snprintf(body, sizeof(body), "car page visible=%d page=%d (%s)",
                           (int)atz_car_page_visible(), atz_car_page_page(),
                           atz_car_page_page() == 1 ? "trip" : "car");
    if (strcmp(action, "hide") == 0 || strcmp(action, "toggle") == 0) {
        written += snprintf(body + written, sizeof(body) - written, " [action=%s]", action);
    }

    // 字段开关（可选）：先 on/off（单项），再 fields（整组）
    if (has_query) {
        char item[64] = {};
        if (httpd_query_key_value(query, "on", item, sizeof(item)) == ESP_OK) {
            atz_car_page_set_field(item, true);
        }
        if (httpd_query_key_value(query, "off", item, sizeof(item)) == ESP_OK) {
            atz_car_page_set_field(item, false);
        }
        if (httpd_query_key_value(query, "fields", item, sizeof(item)) == ESP_OK) {
            atz_car_page_set_fields(item);
        }
    }
    snprintf(body + written, sizeof(body) - written, "\nvisible: %s",
             atz_car_page_get_fields().c_str());
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// 台架用：把**当前界面的真实几何**整棵树 dump 出来（做 UI 记录/核对尺寸用）。
//   http://<设备IP>:8099/layout
// 输出每行：缩进 + 控件类型 + 坐标(x1,y1)-(x2,y2) + 宽x高 + 隐藏标记 + 文本（label）
// 为什么要它：这个板子看不到屏幕、也点不动，所有"距离/尺寸/排列"只能靠真值说话。
// ★ 2026-09-16：缓冲从 4096 加到 8192 —— 车况页 + 行程页两部分加起来已经超过 4KB，
//   超出部分被静默截断（"看不到按钮"就是这么来的，白查了半天）。截断时现在会明说。
esp_err_t LayoutHandler(httpd_req_t* req) {
    static char buf[8192];
    size_t used = 0;
    used += (size_t)snprintf(buf + used, sizeof(buf) - used, "# LVGL tree (screen %dx%d)\n",
                             (int)LV_HOR_RES, (int)LV_VER_RES);
    DumpObjTree(lv_screen_active(), 0, buf, sizeof(buf), &used);
    if (used + 32 < sizeof(buf)) {
        snprintf(buf + used, sizeof(buf) - used, "# end of tree\n");
    }
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// URL 百分号解码（httpd_query_key_value **不做**解码，中文会原样带 %E6%B0%B4 进来）
void UrlDecode(char* s) {
    if (s == nullptr) {
        return;
    }
    char* dst = s;
    for (char* src = s; *src != '\0'; src++) {
        if (*src == '%' && src[1] != '\0' && src[2] != '\0') {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(src[1]);
            const int lo = hex(src[2]);
            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 2;
                continue;
            }
        }
        *dst++ = *src;
    }
    *dst = '\0';
}

// 台架用：往字幕里塞一句话，用来验证「逐字贴弧」排版（不改上游代码）。
//   http://<设备IP>:8099/subtitle?text=你好我是小智   （中文请 URL 编码）
esp_err_t SubtitleHandler(httpd_req_t* req) {
    if (!WriteAllowed(req)) {   // 写操作需要 &key=<ATZ_DEBUG_TOKEN>
        return DenyWrite(req);
    }
    char query[512] = {};
    char value[240] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "text", value, sizeof(value)) == ESP_OK) {
        UrlDecode(value);   // ★ 必须自己解码，httpd 不会替你做
        if (g_display != nullptr) {
            g_display->SetChatMessage("assistant", value);
        }
        char body[300];
        snprintf(body, sizeof(body), "ok: subtitle set to \"%s\"", value);
        return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    }
    if (g_display != nullptr) {
        g_display->SetChatMessage("assistant", "");
    }
    return httpd_resp_send(req, "ok: subtitle cleared", HTTPD_RESP_USE_STRLEN);
}

// ★ PC 端模拟器用的数据入口：把一条"合成车况"注入车况缓存 —— 走的链路与真主表完全一样
//   （espnow_slave_inject_test_packet → obd_data_cache → 转速环/车况条/车况页/告警）。
//   http://<设备IP>:8099/inject?rpm=3200&speed=88&coolant=92&oil=100&intake=38&load=42&tps=27&volt=14.2
//   · 只传想改的字段也行，缺省字段沿用上一次的值（首次用一组合理缺省）
//   · 全部可省略；volt 单位是 V（内部换算成 mV）
//   · http://<设备IP>:8099/inject?stop=1   停止注入（2 秒后数据变"陈旧"，环归零变暗）
//   安全提示：rpm ≥ 6500 会真的触发本地高转告警（会出声），这是设计行为。
// ── 10Hz 重复注入（见 InjectHandler 里的说明）───────────────────────────────
static uint16_t g_inj_rpm = 0;
static uint8_t g_inj_speed = 0;
static int16_t g_inj_coolant = 85, g_inj_oil = 95, g_inj_intake = 30, g_inj_load = 20, g_inj_tps = 10;
static int32_t g_inj_bat_mv = 14200;
static volatile int64_t g_inj_hold_until_us = 0;
static bool g_inj_task_started = false;

void InjectRepeaterTask(void* arg) {
    (void)arg;
    int tick = 0;
    ESP_LOGI(TAG, "inject repeater task running (10Hz while hold is valid)");
    while (true) {
        tick++;
        // 只在"确实在复现"时每 2 秒打一条，避免空闲时刷屏
        if (tick % 20 == 0 && g_inj_hold_until_us != 0 &&
            esp_timer_get_time() < g_inj_hold_until_us) {
            const int64_t left = (g_inj_hold_until_us - esp_timer_get_time()) / 1000;
            ESP_LOGI(TAG, "inject repeater: rpm=%u hold_left=%lld ms", (unsigned)g_inj_rpm,
                     (long long)left);
        }
        if (g_inj_hold_until_us != 0 && esp_timer_get_time() < g_inj_hold_until_us) {
            espnow_slave_inject_test_packet(g_inj_rpm, g_inj_speed, g_inj_coolant, g_inj_oil,
                                            g_inj_intake, g_inj_load, g_inj_tps, g_inj_bat_mv);
        }
        vTaskDelay(pdMS_TO_TICKS(100));   // 10Hz，与真主表同频
    }
}

void atz_inject_repeater_start(void) {
    if (g_inj_task_started) {
        return;
    }
    g_inj_task_started = true;
    const BaseType_t ok = xTaskCreate(InjectRepeaterTask, "atz_inject", 4096, nullptr, 3, nullptr);
    ESP_LOGI(TAG, "inject repeater start: %s", ok == pdPASS ? "ok" : "FAILED");
}
esp_err_t InjectHandler(httpd_req_t* req) {
    if (!WriteAllowed(req)) {   // 写操作需要 &key=<ATZ_DEBUG_TOKEN>
        return DenyWrite(req);
    }
    static uint16_t s_rpm = 0;
    static uint8_t s_speed = 0;
    static int16_t s_coolant = 85, s_oil = 95, s_intake = 30, s_load = 20, s_tps = 10;
    static int32_t s_bat_mv = 14200;

    char query[256] = {};
    char value[32] = {};
    const bool has = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK;

    auto getInt = [&](const char* key, int fallback) -> int {
        if (!has) return fallback;
        if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) return fallback;
        return atoi(value);
    };

    if (getInt("stop", 0) != 0) {
        g_inj_hold_until_us = 0;                 // 停掉 10Hz 重复
        atz_ui_car_inject_stop();                // 顺便停掉设备自带的模拟
        return httpd_resp_send(req, "ok: injection stopped (data goes stale in ~2 s)\n",
                               HTTPD_RESP_USE_STRLEN);
    }

    s_rpm = (uint16_t)getInt("rpm", s_rpm);
    s_speed = (uint8_t)getInt("speed", s_speed);
    s_coolant = (int16_t)getInt("coolant", s_coolant);
    s_oil = (int16_t)getInt("oil", s_oil);
    s_intake = (int16_t)getInt("intake", s_intake);
    s_load = (int16_t)getInt("load", s_load);
    s_tps = (int16_t)getInt("tps", s_tps);
    // 电压用"伏"传进来（14.2），内部按 mV 存
    if (has && httpd_query_key_value(query, "volt", value, sizeof(value)) == ESP_OK) {
        s_bat_mv = (int32_t)(atof(value) * 1000.0f + 0.5f);
        // ★ 防手误（2026-09-16 自己踩的）：有人写成 volt=13900（其实是 mV），
        //   界面就会显示 "13900.00 V" —— 看起来像格式化 bug，其实是输入错了。
        //   12V 车用铅酸/锂电的合理区间是 8~16V，超出就判为"传的是 mV"并自动换算。
        if (s_bat_mv > 16000) {
            ESP_LOGW(TAG, "inject volt=%s looks like mV (%.0f) -> using %.2f V", value,
                     (double)s_bat_mv, (double)(s_bat_mv / 1000));
            s_bat_mv = (int32_t)(s_bat_mv / 1000);
        } else if (s_bat_mv < 8000) {
            ESP_LOGW(TAG, "inject volt=%s out of range, keeping previous %d mV", value,
                     (int)s_bat_mv);
            s_bat_mv = g_inj_bat_mv;
        }
    }

    // ★ 关键：**不在 HTTP 回调里注入一次就完事**，而是交给一个 10Hz 的重复任务。
    //   为什么（2026-09-16 的坑）：设备总共只有 10 个 socket，还要和云端 WebSocket/MQTT 共用。
    //   PC 端按 10Hz 直接打这个端点 → socket 被吃光 → 喊"小智"没反应。
    //   现在 PC 只要 2~3Hz 续一次「保持时长」，设备内部按 10Hz 复现，数据流一样密，socket 少 3~5 倍。
    g_inj_rpm = s_rpm;
    g_inj_speed = s_speed;
    g_inj_coolant = s_coolant;
    g_inj_oil = s_oil;
    g_inj_intake = s_intake;
    g_inj_load = s_load;
    g_inj_tps = s_tps;
    g_inj_bat_mv = s_bat_mv;
    const int hold_ms = getInt("seconds", 3) * 1000;      // 这次注入的有效期（默认 3 秒）
    ESP_LOGI(TAG, "inject: rpm=%u hold=%d ms (task=%d)", (unsigned)s_rpm, hold_ms,
             (int)g_inj_task_started);
    g_inj_hold_until_us = esp_timer_get_time() + (int64_t)hold_ms * 1000;
    atz_inject_repeater_start();                            // 首次调用时起任务，之后是空操作
    espnow_slave_inject_test_packet(s_rpm, s_speed, s_coolant, s_oil, s_intake, s_load, s_tps,
                                    s_bat_mv);
    if (getInt("stop", 0) != 0) {
        g_inj_hold_until_us = 0;
    }

    char body[192];
    snprintf(body, sizeof(body),
             "ok: rpm=%u speed=%u coolant=%d oil=%d intake=%d load=%d tps=%d batt=%.2fV\n",
             (unsigned)s_rpm, (unsigned)s_speed, (int)s_coolant, (int)s_oil, (int)s_intake,
             (int)s_load, (int)s_tps, s_bat_mv / 1000.0);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// 台架用：转速环状态。★ 环已按用户要求删除（ATZ_RPM_RING_ENABLE=0），
// 这个端点保留成**只读**，方便台架确认"现在跑的固件里环到底是编进去了没有"。
//   http://<设备IP>:8099/ring
esp_err_t RingHandler(httpd_req_t* req) {
    char body[160];
    snprintf(body, sizeof(body),
             "ring compiled_in=%d (ATZ_RPM_RING_ENABLE=0 means the feature is removed)\n"
             "ring enabled=%d visible=%d\n",
             (int)ATZ_RPM_RING_ENABLE, (int)atz_rpm_ring_enabled(), (int)atz_rpm_ring_visible());
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}
// 台架用：行程统计（等价语音 self.car.get_trip / self.car.reset_trip / self.car.set_trip_recording）
//   http://<设备IP>:8099/trip                查询
//   http://<设备IP>:8099/trip?reset=1&key=…  清零并开始新一趟
//   http://<设备IP>:8099/trip?rec=1&key=…    开始记录    rec=0 暂停记录
esp_err_t TripHandler(httpd_req_t* req) {
    char query[64] = {};
    char value[16] = {};
    const bool has_query = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK;
    const bool want_reset = has_query &&
                            httpd_query_key_value(query, "reset", value, sizeof(value)) == ESP_OK &&
                            atoi(value) != 0;
    char rec_buf[16] = {};
    const bool want_rec = has_query &&
                          httpd_query_key_value(query, "rec", rec_buf, sizeof(rec_buf)) == ESP_OK;
    if (want_reset || want_rec) {
        if (!WriteAllowed(req)) {   // 清零/开关记录都是写操作
            return DenyWrite(req);
        }
        if (want_reset) {
            atz_trip_reset();
            atz_trip_set_recording(true);   // 「重新开始统计」= 清零后立刻记录
        }
        if (want_rec) {
            atz_trip_set_recording(atoi(rec_buf) != 0);
        }
    }
    atz_trip_stats_t s = {};
    atz_trip_get(&s);
    char body[448];
    snprintf(body, sizeof(body),
             "recording=%d  samples=%lu  session=%lu s\n"
             "max: rpm=%u speed=%u coolant=%d oil=%d intake=%d load=%d\n"
             "min batt=%.2f V   distance~%.2f km   above %d rpm: %lu s\n",
             (int)atz_trip_recording(), (unsigned long)s.samples, (unsigned long)s.uptime_s,
             (unsigned)s.max_rpm, (unsigned)s.max_speed, (int)s.max_coolant, (int)s.max_oil,
             (int)s.max_intake, (int)s.max_load, s.min_bat_mv / 1000.0, s.km_x100 / 100.0,
             ATZ_TRIP_HOT_RPM, (unsigned long)s.hot_s);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// 台架用：量 UI 的真实流畅度 —— 直接读 LVGL 每帧的渲染事件，而不是"看着挺顺"。
//   http://<设备IP>:8099/perf
// 输出：帧率、平均每帧"渲染+刷屏"耗时、每帧重绘像素、各 UI 模块改控件的次数。
// 统计实现见 atz_perf.cc（★ 以前寄生在转速环里，环删掉后会静默变全 0，所以搬出来独立）。
esp_err_t PerfHandler(httpd_req_t* req) {
    PerfSamplerStart();                     // 幂等；后台每 2 秒算一次
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const uint32_t age_ms = (g_perf.at_ms > 0) ? (uint32_t)(now_ms - g_perf.at_ms) : 0xFFFFFFFF;

    char body[400];
    snprintf(body, sizeof(body),
             "# non-blocking: sampled in the background (2 s window), no socket is held\n"
             "age=%lu ms\n"
             "frames=%lu  ->  %lu.%lu fps\n"
             "avg frame busy=%lu us (render+flush)\n"
             "avg redraw=%lu px/frame   peak=%lu px\n"
             "ring timer calls=%lu  ring redraws=%lu   ui redraws=%lu\n"
             "free heap=%lu bytes\n",
             (unsigned long)(age_ms == 0xFFFFFFFF ? 0 : age_ms), (unsigned long)g_perf.frames,
             (unsigned long)(g_perf.fps_x10 / 10), (unsigned long)(g_perf.fps_x10 % 10),
             (unsigned long)g_perf.avg_us, (unsigned long)g_perf.avg_px,
             (unsigned long)g_perf.peak_px, (unsigned long)g_perf.calls,
             (unsigned long)g_perf.pushes, (unsigned long)g_perf.ui_pushes,
             (unsigned long)esp_get_free_heap_size());
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}
// 台架用：查询/调整「表情图」与「顶部时间」的放大比例（立即生效，不用重刷固件）
//   http://<设备IP>:8099/size                     查询当前值
//   http://<设备IP>:8099/size?emoji=150&clock=160  设置并立即生效
esp_err_t SizeHandler(httpd_req_t* req) {
    if (!WriteAllowed(req)) {   // 写操作需要 &key=<ATZ_DEBUG_TOKEN>
        return DenyWrite(req);
    }
    char query[64] = {};
    char value[16] = {};
    int emoji = 0;
    int clock_pct = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "emoji", value, sizeof(value)) == ESP_OK) {
            emoji = atoi(value);
        }
        if (httpd_query_key_value(query, "clock", value, sizeof(value)) == ESP_OK) {
            clock_pct = atoi(value);
        }
    }
    if (emoji > 0 || clock_pct > 0) {
        atz_ui_set_scales(emoji, clock_pct);
    }
    // 回显“当前真实几何”，而不是“素材 128px x 百分比”的推算值 —— 后者和截屏量出来的
    // 数字对不上（素材实际是 128x121，且可见圆比素材框小），会让人以为缩放没生效。
    char body[192];
    atz_ui_describe_sizes(body, sizeof(body));
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

}  // namespace

void atz_shot_server_start(Display* display) {
#if !ATZ_UI_SHOT_SERVER_ENABLE
    (void)display;
    return;
#else
    if (display == nullptr || g_server != nullptr) {
        return;
    }
    g_display = display;
    // 只起一个等待任务，真正的 httpd_start 在拿到 IP 之后（见 StartTask 的说明）
    xTaskCreate(StartTask, "atz_shot_start", 4096, nullptr, 3, nullptr);
#endif
}

void atz_shot_log_ui_state(Display* display) {
    if (display == nullptr) {
        ESP_LOGW(TAG, "ui state: display is null");
        return;
    }
    Theme* theme = display->GetTheme();
    ESP_LOGI(TAG, "ui state: size=%dx%d setup_ui_called=%d theme=%s", display->width(),
             display->height(), (int)display->IsSetupUICalled(),
             theme != nullptr ? theme->name().c_str() : "(null)");
}
