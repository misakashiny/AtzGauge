// atz_hit_zone.cc -- 见头文件里的"为什么需要这个模块"
#include "atz_hit_zone.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>

#define TAG "AtzHitZone"

namespace {

#define ATZ_HIT_ZONE_MAX 8      // 目前最多 6 个（车况页 1 + 行程页 3 + 余量）

struct Zone {
    bool used = false;
    bool enabled = true;      // 停用的区不参与命中（同位置、只在各自页面有效的区靠它区分）
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    atz_hit_cb_t cb = nullptr;
    void* user = nullptr;
    char name[20] = {};
};

Zone g_zones[ATZ_HIT_ZONE_MAX];
SemaphoreHandle_t g_lock = nullptr;

inline bool Inside(const Zone& z, int x, int y) {
    return x >= z.x1 && x <= z.x2 && y >= z.y1 && y <= z.y2;
}

}  // namespace

int atz_hit_zone_add(int x1, int y1, int x2, int y2, atz_hit_cb_t cb, void* user) {
    if (cb == nullptr || x2 < x1 || y2 < y1) {
        return -1;
    }
    if (g_lock == nullptr) {
        g_lock = xSemaphoreCreateMutex();   // 只在启动路径（LVGL 任务）里创建，无竞争
    }
    if (g_lock != nullptr) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
    }
    // 同一矩形 + 同一回调 → 视为"重复注册"，覆盖它（页面重建时不会越注册越多）；
    // ★ 不同回调但矩形相同是**正常情况**：车况页与行程页的底部提示条坐标一样，
    //   两块都要留着（靠 enabled 开关决定哪一块当前生效），所以不能按矩形去重。
    int slot = -1;
    for (int i = 0; i < ATZ_HIT_ZONE_MAX; i++) {
        if (g_zones[i].used && g_zones[i].x1 == x1 && g_zones[i].y1 == y1 &&
            g_zones[i].x2 == x2 && g_zones[i].y2 == y2 && g_zones[i].cb == cb) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < ATZ_HIT_ZONE_MAX; i++) {
            if (!g_zones[i].used) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "no free slot for (%d,%d)-(%d,%d); raise ATZ_HIT_ZONE_MAX", x1, y1, x2, y2);
        if (g_lock != nullptr) {
            xSemaphoreGive(g_lock);
        }
        return -1;
    }
    g_zones[slot].used = true;
    g_zones[slot].x1 = x1;
    g_zones[slot].y1 = y1;
    g_zones[slot].x2 = x2;
    g_zones[slot].y2 = y2;
    g_zones[slot].cb = cb;
    g_zones[slot].user = user;
    if (g_lock != nullptr) {
        xSemaphoreGive(g_lock);
    }
    ESP_LOGI(TAG, "hit zone %d: (%d,%d)-(%d,%d) %dx%d", slot, x1, y1, x2, y2, x2 - x1 + 1,
             y2 - y1 + 1);
    return slot;
}

void atz_hit_zone_set_name(int id, const char* name) {
    if (id < 0 || id >= ATZ_HIT_ZONE_MAX || !g_zones[id].used || name == nullptr) {
        return;
    }
    snprintf(g_zones[id].name, sizeof(g_zones[id].name), "%s", name);
}

int atz_hit_zone_test(int x, int y) {
    if (g_lock != nullptr) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
    }
    int hit = -1;
    for (int i = 0; i < ATZ_HIT_ZONE_MAX; i++) {
        // ★ 必须同时看 used 与 enabled：车况页/行程页底部提示条坐标相同、各注册一份，
        //   靠 enabled 保证只有当前那一页的在生效。
        if (g_zones[i].used && g_zones[i].enabled && Inside(g_zones[i], x, y)) {
            hit = i;   // 后注册的优先（覆盖更细的按钮）
        }
    }
    if (g_lock != nullptr) {
        xSemaphoreGive(g_lock);
    }
    return hit;
}

void atz_hit_zone_set_enabled(int id, bool enabled) {
    if (id < 0 || id >= ATZ_HIT_ZONE_MAX) {
        return;
    }
    if (g_lock != nullptr) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
    }
    if (g_zones[id].used) {
        g_zones[id].enabled = enabled;
    }
    if (g_lock != nullptr) {
        xSemaphoreGive(g_lock);
    }
}

bool atz_hit_zone_enabled(int id) {
    if (id < 0 || id >= ATZ_HIT_ZONE_MAX) {
        return false;
    }
    return g_zones[id].used && g_zones[id].enabled;
}

void atz_hit_zone_fire(int id) {
    if (id < 0 || id >= ATZ_HIT_ZONE_MAX) {
        return;
    }
    atz_hit_cb_t cb = nullptr;
    void* user = nullptr;
    if (g_lock != nullptr) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
    }
    if (g_zones[id].used) {
        cb = g_zones[id].cb;
        user = g_zones[id].user;
    }
    if (g_lock != nullptr) {
        xSemaphoreGive(g_lock);
    }
    if (cb != nullptr) {
        cb(user);   // 不持锁回调：回调里会去拿显示锁，别把这两把锁嵌在一起
    }
}

int atz_hit_zone_count(void) {
    int n = 0;
    for (int i = 0; i < ATZ_HIT_ZONE_MAX; i++) {
        if (g_zones[i].used && g_zones[i].cb != nullptr) {
            n++;
        }
    }
    return n;
}

bool atz_hit_zone_info(int i, int* x1, int* y1, int* x2, int* y2, const char** name) {
    if (i < 0 || i >= ATZ_HIT_ZONE_MAX || !g_zones[i].used) {
        return false;
    }
    if (x1 != nullptr) *x1 = g_zones[i].x1;
    if (y1 != nullptr) *y1 = g_zones[i].y1;
    if (x2 != nullptr) *x2 = g_zones[i].x2;
    if (y2 != nullptr) *y2 = g_zones[i].y2;
    if (name != nullptr) {
        *name = g_zones[i].name[0] != '\0' ? g_zones[i].name : "(unnamed)";
    }
    return true;
}
