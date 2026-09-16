// atz_shot.cc -- 本地截图端点实现（见 atz_shot.h 的说明）

#include "atz_shot.h"
#include "atz_ui_config.h"

#include "display.h"
#include "atz_car_page.h"
#include "atz_rpm_ring.h"
#include "atz_touch.h"
#include "atz_ui.h"
#include "jpg/image_to_jpeg.h"
#include "lvgl_theme.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
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
esp_err_t CarPageHandler(httpd_req_t* req);
esp_err_t SizeHandler(httpd_req_t* req);
esp_err_t PerfHandler(httpd_req_t* req);
esp_err_t LayoutHandler(httpd_req_t* req);
esp_err_t SubtitleHandler(httpd_req_t* req);

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

esp_err_t HealthHandler(httpd_req_t* req) {
    return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

// 台架用：从 PC 直接切主题，不必对着设备说话。
//   http://<设备IP>:8099/theme?name=atz-day
// 主题名会被写进 NVS（Display::SetTheme 的行为），所以和语音切换等价。
esp_err_t ThemeHandler(httpd_req_t* req) {
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
            // ★ 必须 ≥ 实际注册的端点个数：httpd_register_uri_handler 超限时只返回
            //   ESP_ERR_HTTPD_HANDLERS_FULL，**不会**报错也不会崩，表现为该 URL 直接 404。
            //   （踩过一次：设 4 而注册了 5 个 → /touch 一直 404。）
            config.max_uri_handlers = 12;

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
            httpd_uri_t subtitle_uri = {
                .uri = "/subtitle",
                .method = HTTP_GET,
                .handler = SubtitleHandler,
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
            if (httpd_register_uri_handler(g_server, &subtitle_uri) != ESP_OK) {
                ESP_LOGE(TAG, "/subtitle registration failed (max_uri_handlers too small?)");
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
esp_err_t TouchHandler(httpd_req_t* req) {
    char query[64] = {};
    char value[16] = {};
    int seconds = 5;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "seconds", value, sizeof(value)) == ESP_OK) {
        seconds = atoi(value);
    }
    char report[256] = {};
    atz_touch_watch(seconds, report, sizeof(report));   // 阻塞 seconds 秒（调试端点，可接受）
    return httpd_resp_send(req, report, HTTPD_RESP_USE_STRLEN);
}

// 台架用：显示/隐藏「车况」整屏页面（等价于语音工具 self.ui.show_car_page）
//   http://<设备IP>:8099/carpage?action=show | hide | toggle
//   http://<设备IP>:8099/carpage?fields=rpm,coolant   只显示这几项（all = 全部）
//   http://<设备IP>:8099/carpage?on=intake&off=load   单项开关（等价于语音 set_car_page_field）
esp_err_t CarPageHandler(httpd_req_t* req) {
    char query[192] = {};
    char value[128] = {};
    const char* action = "show";
    const bool has_query = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK;
    if (has_query && httpd_query_key_value(query, "action", value, sizeof(value)) == ESP_OK) {
        action = value;
    }
    if (strcmp(action, "hide") == 0) {
        atz_car_page_hide();
    } else if (strcmp(action, "toggle") == 0) {
        atz_car_page_toggle();
    } else {
        atz_car_page_show();
    }

    char body[320];
    int written = snprintf(body, sizeof(body), "ok: car page -> %s", action);

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
esp_err_t LayoutHandler(httpd_req_t* req) {
    static char buf[4096];
    size_t used = 0;
    used += (size_t)snprintf(buf + used, sizeof(buf) - used, "# LVGL tree (screen %dx%d)\n",
                             (int)LV_HOR_RES, (int)LV_VER_RES);
    DumpObjTree(lv_screen_active(), 0, buf, sizeof(buf), &used);
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

// 台架用：量 UI 的真实流畅度 —— 直接读 LVGL 每帧的渲染事件，而不是"看着挺顺"。
//   http://<设备IP>:8099/perf?seconds=5
// 输出：帧率、平均每帧"渲染+刷屏"耗时、环定时器实际调用次数与真正触发重绘的次数。
// 为什么要有这个：环的包围盒是 340×340，中间压着带缩放的表情图，每重绘一次都很贵；
// 没有数字就只能猜（"是不是 CPU 不够""是不是缓冲区太小"）。
esp_err_t PerfHandler(httpd_req_t* req) {
    char query[64] = {};
    char value[16] = {};
    int seconds = 5;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "seconds", value, sizeof(value)) == ESP_OK) {
        seconds = atoi(value);
    }
    if (seconds < 1) {
        seconds = 1;
    }
    if (seconds > 20) {
        seconds = 20;
    }

    uint32_t f0 = 0, c0 = 0, p0 = 0;
    uint64_t b0 = 0, w0 = 0, px0 = 0;
    uint32_t im0 = 0;
    atz_rpm_ring_perf_read(&f0, &b0, &c0, &p0);
    atz_rpm_ring_perf_ext(&w0, &px0, &im0);
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    uint32_t f1 = 0, c1 = 0, p1 = 0;
    uint64_t b1 = 0, w1 = 0, px1 = 0;
    uint32_t im1 = 0;
    atz_rpm_ring_perf_read(&f1, &b1, &c1, &p1);
    atz_rpm_ring_perf_ext(&w1, &px1, &im1);

    const uint32_t frames = f1 - f0;
    const uint32_t calls = c1 - c0;
    const uint32_t pushes = p1 - p0;
    const uint64_t busy_us = b1 - b0;
    const uint64_t worst_us = w1 - w0;    // 本采样窗口内最慢的一帧（累计值是"历史最大"，这里取增量近似）
    const uint64_t inv_px = px1 - px0;
    // 每帧平均耗时（含软件渲染 + QSPI 刷屏）；帧率 = 帧数 / 采样秒数
    const uint32_t avg_us = (frames > 0) ? (uint32_t)(busy_us / frames) : 0;
    const uint32_t avg_px = (frames > 0) ? (uint32_t)(inv_px / frames) : 0;
    const uint32_t fps_x10 = (uint32_t)((uint64_t)frames * 10 / (uint32_t)seconds);
    const uint32_t heap = (uint32_t)esp_get_free_heap_size();

    char body[400];
    snprintf(body, sizeof(body),
             "sample=%d s\n"
             "frames=%lu  ->  %lu.%lu fps\n"
             "avg frame busy=%lu us (render+flush)   worst=%lu us\n"
             "avg redraw=%lu px/frame   peak=%lu px\n"
             "ring timer calls=%lu  real redraws=%lu\n"
             "free heap=%lu bytes\n",
             seconds, (unsigned long)frames, (unsigned long)(fps_x10 / 10),
             (unsigned long)(fps_x10 % 10), (unsigned long)avg_us, (unsigned long)worst_us,
             (unsigned long)avg_px, (unsigned long)im1, (unsigned long)calls,
             (unsigned long)pushes, (unsigned long)heap);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// 台架用：查询/调整「表情图」与「顶部时间」的放大比例（立即生效，不用重刷固件）
//   http://<设备IP>:8099/size                     查询当前值
//   http://<设备IP>:8099/size?emoji=150&clock=160  设置并立即生效
esp_err_t SizeHandler(httpd_req_t* req) {
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
