#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"

// ESP-NOW 从表（本项目新增）：接收仪表主表广播的车况。见 main/espnow_slave/。
#include "espnow_link.h"
#include "car_alarm.h"
#include "car_status_tool.h"
#include "device_state.h"

// 主界面定制（本项目新增）：主题包 + 车况条 + 语音控制。见同目录 atz_ui.* 与 atz_ui_config.h。
#include "atz_ui.h"
#include "atz_ui_config.h"   // ATZ_UI_ENABLE 总开关
// 台架调试：本地截图端点（http://<设备IP>:8099/shot.jpg）。见 atz_shot.h。
#include "atz_shot.h"
// 「车况」整屏页面（语音进入）。见 atz_car_page.h。
#include "atz_car_page.h"
#include "settings.h"

#include <esp_log.h>
#include "i2c_device.h"
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include <esp_lcd_touch_cst816s.h>   // ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS
#include <esp_timer.h>
#include "esp_io_expander_tca9554.h"
#include <freertos/FreeRTOS.h>   // vTaskPrioritySet / xTaskGetHandle（LVGL 任务提优先级）
#include <freertos/task.h>

#define TAG "waveshare_lcd_1_85"

#define LCD_OPCODE_WRITE_CMD        (0x02ULL)
#define LCD_OPCODE_READ_CMD         (0x0BULL)
#define LCD_OPCODE_WRITE_COLOR      (0x32ULL)

// ── Touch: tap the screen to start a conversation ─────────────────────────────
// The CST816S shares nothing with the LCD: it hangs off its own I2C bus
// (I2C_NUM_1, see TP_* in config.h) and is reset by the TCA9554 together with
// the panel. We poll it from a small task instead of handing it to
// esp_lvgl_port, because esp_lvgl_port_touch.c wraps every read in
// ESP_ERROR_CHECK() while the CST816S NACKs often enough to turn that into a
// reboot loop (开发参考/17 §9).
//
// Reads go through the raw i2c_master API, not esp_lcd_panel_io:
//   * a sleeping CST816S does not ACK its address, so most idle polls fail --
//     that is normal, not an error;
//   * i2c_master.c logs a NACK at DEBUG (s_i2c_err_log_print), while
//     panel_io_i2c_rx_buffer() logs every failure at ERROR -- measured 1094
//     ERROR lines in 45s of idle polling before the switch, now zero.
// Timeouts and bus faults still surface as ERROR from i2c_master, so real
// trouble is not hidden. Coordinates are not needed for a tap trigger, so this
// stays a plain 5-byte register read, and keeping the panel out of LVGL means
// the task never takes an LVGL lock.
#define TP_DATA_START_REG   (0x02)  // FingerNum, XH, XL, YH, YL (CST816S datasheet)
#define TP_CHIP_ID_REG      (0xA7)  // answers even while untouched -> used as the probe
#define TP_POLL_INTERVAL_MS (30)
#define TP_CONFIRM_SAMPLES  (2)     // ~60ms of contact before a tap is accepted
#define TP_PROBE_ATTEMPTS   (10)    // the chip needs a moment after the TCA9554 reset
#define TP_PROBE_GAP_MS     (100)
#define TP_REPROBE_INTERVAL_MS (5000)  // 开机没找到时，后台每 5s 静默重试一次
// ★ A/B 开关：用 09-14 21:54 那版【实测可用】的实现，还是用后来自我重构的版本。
//   1 = 复刻可用版：esp_lcd_panel_io 路径（无限超时）、不碰 GPIO4、只试 I2C_NUM_1、单次探测
//   0 = 现版本：原生 i2c_master（50ms 超时）、配 GPIO4 上拉、双总线探测、重试
//   之所以留着这个开关：那一版实测读到过真实触摸（两次点击都触发了对话），
//   之后的版本再没读到过 —— 用它做对照，才能判断是"回归"还是"硬件不应答"。
#define TP_LEGACY_PATH      1
#define TP_DIAG             0       // 1 = print a 5s read counter heartbeat (bring-up aid)

// Start the car-alarm monitor.
//
// Why a task instead of calling car_alarm_start() straight from the board
// constructor: the alarm self-test fires a REAL Application::Alert(), which plays
// an embedded ogg and touches the display. Doing that before Application/audio
// finished booting would be unsafe. Gate on the device state leaving
// kDeviceStateStarting (measured: audio is up at ~1.2s, state moves to
// wifi_configuring at ~2.8s) instead of a magic sleep.
static void atz_alarm_boot_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 120; i++) {          // up to ~60s
        DeviceState st = Application::GetInstance().GetDeviceState();
        if (st != kDeviceStateUnknown && st != kDeviceStateStarting) {
            car_alarm_start();
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "device never left kDeviceStateStarting; starting alarm monitor anyway");
    car_alarm_start();
    vTaskDelete(NULL);
}

static const st77916_lcd_init_cmd_t vendor_specific_init_new[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){0x00}, 1, 0},
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0x29, (uint8_t []){0x00}, 1, 0},  
};
class AtzGaugeBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    esp_io_expander_handle_t io_expander = NULL;
    LcdDisplay* display_;
    AtzLcdDisplay* atz_display_ = nullptr;
    button_handle_t boot_btn, pwr_btn;
    button_driver_t* boot_btn_driver_ = nullptr;
    button_driver_t* pwr_btn_driver_ = nullptr;
    i2c_master_bus_handle_t tp_bus_ = nullptr;
    i2c_master_dev_handle_t tp_dev_ = nullptr;
    esp_lcd_panel_io_handle_t tp_io_ = nullptr;   // TP_LEGACY_PATH=1 时用这条路径
    static AtzGaugeBoard* instance_;
    static void TouchPollTask(void* arg);

    // One read starting at `reg`. The error is returned as-is; the caller decides
    // what a failure means (idle NACK vs. a real bus fault).
    esp_err_t TouchReadReg(uint8_t reg, uint8_t* data, size_t len) {
        return i2c_master_transmit_receive(tp_dev_, &reg, 1, data, len, pdMS_TO_TICKS(50));
    }

    esp_err_t TouchRead(uint8_t* data, size_t len) {
#if TP_LEGACY_PATH
        // 复刻 09-14 21:54 那版【实测可用】的读取路径：
        // 走 esp_lcd_panel_io，内部超时是 -1（无限等待），而不是我们自己的 50ms。
        if (tp_io_ != nullptr) {
            return esp_lcd_panel_io_rx_param(tp_io_, TP_DATA_START_REG, data, len);
        }
        return ESP_ERR_INVALID_STATE;
#else
        return TouchReadReg(TP_DATA_START_REG, data, len);
#endif
    }

    bool TouchAttached() const {
#if TP_LEGACY_PATH
        return tp_io_ != nullptr;
#else
        return tp_dev_ != nullptr;
#endif
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = I2C_SDA_IO,
            .scl_io_num = I2C_SCL_IO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }
    
    void InitializeTca9554(void) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, I2C_ADDRESS, &io_expander);
        if(ret != ESP_OK)
            ESP_LOGE(TAG, "TCA9554 create returned error");        

        // EXIO0 与 EXIO1 一起驱动：厂商 demo 的注释说这两个脚分别复位 LCD 与触摸板。
        // 下面这几行读回电平的写法来自厂商 demo，留着备查：
        // uint32_t input_level_mask = 0;
        // ret = esp_io_expander_get_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, &input_level_mask);
        // ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3, IO_EXPANDER_OUTPUT);
        // ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3, 1);
        // ret = esp_io_expander_print_state(io_expander);
        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, IO_EXPANDER_OUTPUT);
        ESP_ERROR_CHECK(ret);   // 原来这行的检查被乱码注释吞掉了，等于没查
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 1);
        ESP_ERROR_CHECK(ret);   // 释放复位（高电平）
        vTaskDelay(pdMS_TO_TICKS(300));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 0);
        ESP_ERROR_CHECK(ret);   // 拉低（复位）
        vTaskDelay(pdMS_TO_TICKS(300));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 1);
        ESP_ERROR_CHECK(ret);   // 再释放 → LCD 与触摸控制器开始工作
        ESP_ERROR_CHECK(ret);
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize QSPI bus");

        const spi_bus_config_t bus_config = TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(QSPI_PIN_NUM_LCD_PCLK,
                                                                        QSPI_PIN_NUM_LCD_DATA0,
                                                                        QSPI_PIN_NUM_LCD_DATA1,
                                                                        QSPI_PIN_NUM_LCD_DATA2,
                                                                        QSPI_PIN_NUM_LCD_DATA3,
                                                                        QSPI_LCD_H_RES * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    void Initializest77916Display() {

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGI(TAG, "Install panel IO");
        
        esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = QSPI_PIN_NUM_LCD_CS,               
            .dc_gpio_num = GPIO_NUM_NC,
            .spi_mode = 0,                     
            .pclk_hz = 3 * 1000 * 1000,      
            .trans_queue_depth = 10,            
            .on_color_trans_done = NULL,                            
            .user_ctx = NULL,                   
            .lcd_cmd_bits = 32,                 
            .lcd_param_bits = 8,                
            .flags = {                          
            .dc_low_on_data = 0,            
            .octal_mode = 0,                
            .quad_mode = 1,                 
            .sio_mode = 0,                  
            .lsb_first = 0,                 
            .cs_high_active = 0,            
            },                                  
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io));

        ESP_LOGI(TAG, "Install ST77916 panel driver");
        
        st77916_vendor_config_t vendor_config = {
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        
        printf("-------------------------------------- Version selection -------------------------------------- \r\n");
        esp_err_t ret;
        int lcd_cmd = 0x04;
        uint8_t register_data[4]; 
        size_t param_size = sizeof(register_data);
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_READ_CMD << 24;  // Use the read opcode instead of write
        ret = esp_lcd_panel_io_rx_param(panel_io, lcd_cmd, register_data, param_size); 
        if (ret == ESP_OK) {
            printf("Register 0x04 data: %02x %02x %02x %02x\n", register_data[0], register_data[1], register_data[2], register_data[3]);
        } else {
            printf("Failed to read register 0x04, error code: %d\n", ret);
        } 
        // panel_io_spi_del(io_handle);
        io_config.pclk_hz = 80 * 1000 * 1000;
        if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io) != ESP_OK){
            printf("Failed to set LCD communication parameters -- SPI\r\n");
            return ;
        }
        printf("LCD communication parameters are set successfully -- SPI\r\n");
        
        // Check register values and configure accordingly
        if (register_data[0] == 0x00 && register_data[1] == 0x7F && register_data[2] == 0x7F && register_data[3] == 0x7F) {
            // Handle the case where the register data matches this pattern
            printf("Vendor-specific initialization for case 1.\n");
        }
        else if (register_data[0] == 0x00 && register_data[1] == 0x02 && register_data[2] == 0x7F && register_data[3] == 0x7F) {
            // Provide vendor-specific initialization commands if register data matches this pattern
            vendor_config.init_cmds = vendor_specific_init_new;
            vendor_config.init_cmds_size = sizeof(vendor_specific_init_new) / sizeof(st77916_lcd_init_cmd_t);
            printf("Vendor-specific initialization for case 2.\n");
        }
        printf("------------------------------------- End of version selection------------------------------------- \r\n");
 
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL;
        panel_config.reset_gpio_num = QSPI_PIN_NUM_LCD_RST;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

#if ATZ_UI_ENABLE
        // ★ 必须在构造显示对象之前注册主题：LcdDisplay 的构造函数会立刻从 NVS 读上次的
        //   主题名并查表，若那时我们的主题还没注册，current_theme_ 会是 nullptr。
        atz_ui_register_themes();
#else
        // ATZ_UI_ENABLE=0：界面用官方原版（不加主题包/车况条），但要把 NVS 里的主题名
        // 复位成上游默认值 —— 否则它可能还指向上次存的 "atz-night" 之类，而我们的主题在
        // 关闭状态下没注册，查表得到 nullptr，上游 SetupUI() 解引用就会崩。
        {
            Settings display_settings("display", true);
            std::string saved_theme = display_settings.GetString("theme", "light");
            if (saved_theme != "light" && saved_theme != "dark") {
                display_settings.SetString("theme", "light");
                ESP_LOGW(TAG, "ATZ_UI_ENABLE=0 -> NVS theme '%s' reset to 'light'",
                         saved_theme.c_str());
            }
        }
#endif
        // 显示对象统一用 AtzLcdDisplay：
        //   * ATZ_UI_ENABLE=1 时它负责主题包与车况条；
        //   * 无论开关如何，它都负责**尺寸微调**（表情放大 / 顶部时间放大）——
        //     这两项只改上游已有控件的大小，所以不跟着总开关走。
        atz_display_ = new AtzLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        display_ = atz_display_;
        atz_ui_bind(atz_display_);   // 记住实例，供运行时调"表情/时间"尺寸用
    }
 

    void InitializeButtonsCustom() {
        gpio_reset_pin(BOOT_BUTTON_GPIO);                                     
        gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);   
        gpio_reset_pin(PWR_BUTTON_GPIO);                                     
        gpio_set_direction(PWR_BUTTON_GPIO, GPIO_MODE_INPUT);   
        gpio_reset_pin(PWR_Control_PIN);                                     
        gpio_set_direction(PWR_Control_PIN, GPIO_MODE_OUTPUT);    
        // gpio_set_level(PWR_Control_PIN, false);
        gpio_set_level(PWR_Control_PIN, true); 
    }
    void InitializeButtons() {
        instance_ = this;
        InitializeButtonsCustom();

        // Boot Button
        button_config_t boot_btn_config = {
            .long_press_time = 2000,
            .short_press_time = 0
        };
        boot_btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        boot_btn_driver_->enable_power_save = false;
        boot_btn_driver_->get_key_level = [](button_driver_t *button_driver) -> uint8_t {
            return !gpio_get_level(BOOT_BUTTON_GPIO);
        };
        ESP_ERROR_CHECK(iot_button_create(&boot_btn_config, boot_btn_driver_, &boot_btn));
        iot_button_register_cb(boot_btn, BUTTON_SINGLE_CLICK, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<AtzGaugeBoard*>(usr_data);
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                self->EnterWifiConfigMode();
                return;
            }
            // 车况页开着时，BOOT 键先用来关页面（不然按一下会开始说话、页面还挡着）
            if (atz_car_page_visible()) {
                atz_car_page_hide();
                return;
            }
            app.ToggleChatState();
        }, this);

        // Power Button
        button_config_t pwr_btn_config = {
            .long_press_time = 5000,
            .short_press_time = 0
        };
        pwr_btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        pwr_btn_driver_->enable_power_save = false;
        pwr_btn_driver_->get_key_level = [](button_driver_t *button_driver) -> uint8_t {
            return !gpio_get_level(PWR_BUTTON_GPIO);
        };
        ESP_ERROR_CHECK(iot_button_create(&pwr_btn_config, pwr_btn_driver_, &pwr_btn));
        iot_button_register_cb(pwr_btn, BUTTON_LONG_PRESS_START, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<AtzGaugeBoard*>(usr_data);
            if(self->GetBacklight()->brightness() > 0) {
                self->GetBacklight()->SetBrightness(0);
                gpio_set_level(PWR_Control_PIN, false);
            }
            else {
                self->GetBacklight()->RestoreBrightness();
                gpio_set_level(PWR_Control_PIN, true);
            }
        }, this);
    }

    // Attach to the controller on one bus and ask for its chip id. Returns true and
    // hands back the device handle when it answers.
    //
    // The chip id (not the touch data) is the right probe: it must answer even while
    // untouched, so one read decides "is the controller alive" without a finger on the
    // glass. Retries are needed because a CST816S that is still booting NACKs its own
    // address -- a single shot produced a misleading "not found" during bring-up.
    bool TouchTryBus(i2c_master_bus_handle_t bus, const char* label,
                     i2c_master_dev_handle_t* out_dev, int attempts, bool verbose) {
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
            .scl_speed_hz = 400 * 1000,
        };
        i2c_master_dev_handle_t dev = nullptr;
        if (i2c_master_bus_add_device(bus, &dev_config, &dev) != ESP_OK) {
            if (verbose) {
                ESP_LOGW(TAG, "touch: cannot attach to %s", label);
            }
            return false;
        }
        for (int attempt = 1; attempt <= attempts; attempt++) {
            uint8_t reg = TP_CHIP_ID_REG, id = 0;
            if (i2c_master_transmit_receive(dev, &reg, 1, &id, 1, pdMS_TO_TICKS(50)) == ESP_OK) {
                ESP_LOGI(TAG, "CST816S found on %s (chip id 0x%02X, attempt %d)", label, id, attempt);
                *out_dev = dev;
                return true;
            }
            if (attempt < attempts) {
                vTaskDelay(pdMS_TO_TICKS(TP_PROBE_GAP_MS));
            }
        }
        i2c_master_bus_rm_device(dev);
        if (verbose) {
            ESP_LOGW(TAG, "CST816S silent on %s", label);
        }
        return false;
    }

    // 触摸唤醒：点一下屏幕就开始对话（仅空闲时）。
    //
    // 总线不写死。1.85 系列不同批次接线不同：官方 1.85 头文件写的是独立 I2C_NUM_1
    // (GPIO1/3)，而本仓库的 1.85b 把触摸挂在 TCA9554 那条共享总线上。官方 1.85 板型根本
    // 没有触摸代码，也就是说那些 TP_* 引脚从未被上游验证过。所以两条候选总线都试，
    // 谁应答就用谁；两条都不应答就整体关掉本功能，不留 30ms 一次的无用轮询。
    // 尝试接上触摸控制器：建总线（若还没建）→ 试两条候选总线 → 谁应答用谁。
    // 可反复调用：找不到就把专用总线拆掉，下次重新来（进程里不留半成品状态）。
    //
    // attempts/verbose 是为"开机后周期性重试"准备的：开机时重试 10 次并打印日志，
    // 后台重试只要 1 次且安静，免得每 5 秒刷一行 WARN。
    bool TouchAttach(int attempts, bool verbose) {
        if (TouchAttached()) {
            return true;
        }

#if TP_LEGACY_PATH
        // ── 复刻可用版 ────────────────────────────────────────────────────────
        // 只做当时做过的三件事：建 I2C_NUM_1 总线、挂 panel-IO、读一次数据寄存器。
        // 刻意【不】配置 GPIO4、【不】试第二条总线、【不】重试 —— 因为那一版就是这样，
        // 而它读到了真实触摸。任何额外动作都可能是有害的差异。
        (void)attempts;
        if (tp_bus_ == nullptr) {
            i2c_master_bus_config_t bus_config = {
                .i2c_port = TP_PORT,
                .sda_io_num = TP_PIN_NUM_SDA,
                .scl_io_num = TP_PIN_NUM_SCL,
                .clk_source = I2C_CLK_SRC_DEFAULT,
                .glitch_ignore_cnt = 7,
                .flags = {
                    .enable_internal_pullup = 1,
                },
            };
            if (i2c_new_master_bus(&bus_config, &tp_bus_) != ESP_OK) {
                tp_bus_ = nullptr;
                if (verbose) {
                    ESP_LOGW(TAG, "touch(legacy): i2c bus init failed");
                }
                return false;
            }
        }
        esp_lcd_panel_io_i2c_config_t io_cfg = {};
        io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;
        io_cfg.scl_speed_hz = 400 * 1000;
        io_cfg.control_phase_bytes = 1;
        io_cfg.dc_bit_offset = 0;
        io_cfg.lcd_cmd_bits = 8;
        io_cfg.lcd_param_bits = 0;
        io_cfg.flags.disable_control_phase = 1;
        if (esp_lcd_new_panel_io_i2c(tp_bus_, &io_cfg, &tp_io_) != ESP_OK) {
            tp_io_ = nullptr;
            if (verbose) {
                ESP_LOGW(TAG, "touch(legacy): panel io create failed");
            }
            return false;
        }
        // 压掉 panel-IO 层的 NACK 刷屏。
        //
        // 背景：CST816S 空闲时休眠、不应答自己的地址，于是 panel_io_i2c_rx_buffer() 每次
        // 失败都打一行 ERROR —— 30ms 轮询下实测 20 秒刷了 347 行（45 秒 1094 行）。
        // 当初就是为了这个才把读取路径换成原生 i2c_master，结果把功能换坏了（见下方说明）。
        // 现在改成：**保留能用的读取路径，只把这个 tag 的日志关掉**。
        // 真实故障仍可从 /touch 自检端点和 TP_DIAG 计数看出来，不是把问题藏起来。
        esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
        uint8_t probe[5] = {};
        esp_err_t err = esp_lcd_panel_io_rx_param(tp_io_, TP_DATA_START_REG, probe, sizeof(probe));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "touch(legacy): CST816S answered (finger_num=%u)", probe[0]);
            return true;
        }
        // 连读失败也和当时一样：不删对象，交给轮询任务继续试（当时就是这样，且随后读到了触摸）
        ESP_LOGW(TAG, "touch(legacy): CST816S did not answer (%s); will keep polling",
                 esp_err_to_name(err));
        return true;
#else
        // Candidate 1: the dedicated bus the 1.85 header documents.
        if (tp_bus_ == nullptr || tp_bus_ == i2c_bus_) {
            i2c_master_bus_config_t bus_config = {
                .i2c_port = TP_PORT,
                .sda_io_num = TP_PIN_NUM_SDA,
                .scl_io_num = TP_PIN_NUM_SCL,
                .clk_source = I2C_CLK_SRC_DEFAULT,
                .glitch_ignore_cnt = 7,
                .flags = {
                    .enable_internal_pullup = 1,
                },
            };
            if (i2c_new_master_bus(&bus_config, &tp_bus_) != ESP_OK) {
                tp_bus_ = nullptr;
                if (verbose) {
                    ESP_LOGW(TAG, "touch: dedicated bus init failed");
                }
            }
        }
        if (tp_bus_ != nullptr && tp_bus_ != i2c_bus_) {
            if (TouchTryBus(tp_bus_, "dedicated bus (config.h TP_*)", &tp_dev_, attempts, verbose)) {
                return true;
            }
        }

        // Candidate 2: the bus shared with the TCA9554 (1.85b-style wiring).
        if (TouchTryBus(i2c_bus_, "shared bus 0 (TCA9554 bus)", &tp_dev_, attempts, verbose)) {
            if (tp_bus_ != nullptr && tp_bus_ != i2c_bus_) {
                i2c_del_master_bus(tp_bus_);
            }
            tp_bus_ = i2c_bus_;
            return true;
        }

        if (tp_bus_ != nullptr && tp_bus_ != i2c_bus_) {
            i2c_del_master_bus(tp_bus_);
            tp_bus_ = nullptr;
        }
        return false;
#endif  // TP_LEGACY_PATH
    }

    // 开机后仍不应答时，由这个任务每 5 秒静默重试一次。
    // 为什么要它：CST816S 这类控制器"睡着就不 ACK"，而开机探测只有一次 ——
    // 一旦开机那一瞬没答应，功能就被永久禁用了，只能重启。有了它，
    // 面板任何时候恢复（重新上电、芯片自己醒来）都能自动接上，不用再刷固件。
    static void TouchProberTask(void* arg) {
        auto* self = static_cast<AtzGaugeBoard*>(arg);
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(TP_REPROBE_INTERVAL_MS));
            if (self->TouchAttach(1, false)) {
                ESP_LOGI(TAG, "touch controller appeared after boot; tap-to-talk enabled");
                xTaskCreate(TouchPollTask, "atz_touch_poll", 3072, self, 3, nullptr);
                vTaskDelete(nullptr);
                return;
            }
        }
    }

    void InitializeTouchWake() {
        ESP_LOGI(TAG, "Initialize touch panel for tap-to-talk (dedicated bus I2C%d sda=%d scl=%d)",
                 (int)TP_PORT, (int)TP_PIN_NUM_SDA, (int)TP_PIN_NUM_SCL);

        // INT is not used: the poll task reads the data register instead.
#if !TP_LEGACY_PATH
        // ★ 可用版（21:54）里【没有】这段。加它是想"规范一点"，但它属于与可用版的差异之一，
        //   所以 TP_LEGACY_PATH=1 时不执行 —— 对照实验必须只有"当时做过的事"。
        gpio_set_direction(TP_PIN_NUM_INT, GPIO_MODE_INPUT);
        gpio_set_pull_mode(TP_PIN_NUM_INT, GPIO_PULLUP_ONLY);
#else
        ESP_LOGI(TAG, "touch(legacy): GPIO4/TP_INT left untouched (as in the working build)");
#endif

        // ★ 不要再单独脉冲 EXIO1！
        //
        // 曾经在这里加过一次"给触摸控制器一个独立复位脉冲"（EXIO1 拉低 20ms 再放开），
        // 想救活不应答的 CST816S。结果实测屏幕变成【背光亮、画面全黑】：
        // InitializeTca9554() 是把 EXIO0/EXIO1 一起脉冲的（厂商注释说这俩分别复位
        // LCD 与触摸板），而显示初始化在它之后完成 —— 在显示初始化**之后**再动 EXIO1，
        // 会把已经配好的 LCD 再复位一次，面板丢掉初始化序列，自然什么都不画。
        // 这个脉冲对触摸问题也毫无帮助（两总线都试过，结果是 NACK），所以直接去掉。

        if (TouchAttach(TP_PROBE_ATTEMPTS, true)) {
            xTaskCreate(TouchPollTask, "atz_touch_poll", 3072, this, 3, nullptr);
            return;
        }

        ESP_LOGW(TAG, "touch controller not found on any bus; tap-to-talk disabled for now "
                      "(will retry every %d ms)",
                 TP_REPROBE_INTERVAL_MS);
        xTaskCreate(TouchProberTask, "atz_touch_probe", 3072, this, 2, nullptr);
    }

    // 台架自检：监听 seconds 秒，统计 I2C 读到多少次有效样本、最大手指数。
    // 定义在 public 段（见下方 TouchWatch），这里只留个说明。
    // 用途：用户点屏幕时用浏览器访问 /touch 就能知道"面板到底答不答"——
    // 这是区分"固件逻辑问题"与"面板不应答"的唯一可靠手段。

    // 把车况查询注册成 MCP 工具，让用户能用自然语言问"水温多少"。
    // 工具实现在 main/espnow_slave/car_status_tool.cc，保持板级代码只做装配。
    void InitializeTools() {
        car_status_tool_register();
        // 语音工具：车况注入/转速模拟、顶栏图标开关（**与 ATZ_UI_ENABLE 无关**）。
        // 主题切换、车况条开关在函数内部用 #if ATZ_UI_ENABLE 单独保护。
        // 教训：以前整个调用被 ATZ_UI_ENABLE 包住，导致官方界面下说"模拟一下转速"没有工具可调。
        atz_ui_register_tools(atz_display_);
        // 台架调试：本地截图端点，方便在 PC 上直接看屏幕（与界面定制无关，独立开关）
        atz_shot_server_start(display_);
        // 「车况」整屏页面：语音"显示车况"进入（独立于 ATZ_UI_ENABLE，官方界面下也能用）
        atz_car_page_init(display_);
    }

public:
    AtzGaugeBoard() {   
        // 开机第一条日志就把"这是哪一版固件"说清楚：名字、阶段、构建时间、关键开关。
        // 台架上排查时最常问的就是"设备里现在跑的是哪版"，以前只能比对日志猜。
        ESP_LOGI(TAG, "=== firmware: %s %s | built %s | UI=%d ring=%d arc_text=%d ===",
                 ATZ_FW_NAME, ATZ_FW_STAGE, ATZ_FW_BUILT, ATZ_UI_ENABLE, ATZ_RPM_RING_ENABLE,
                 ATZ_ARC_TEXT_ENABLE);
        InitializeI2c();
        InitializeTca9554();
        InitializeSpi();
        Initializest77916Display();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();

        // 点屏唤醒对话。放在 TCA9554 复位之后：触摸控制器与 LCD 一起被它复位。
        InitializeTouchWake();

        // MCP：注册 self.car.get_status，支持语音问答车况
        InitializeTools();

        // 启动 ESP-NOW 从表。异步：等 STA 真正连上路由器（信道确定）后才初始化，
        // 因为 ESP-NOW 跟随 STA 信道，而主表硬编码在信道 1。
        // 不阻塞构造函数，符合“不要把主事件循环卡住”的约束。
        espnow_slave_start_async();

        // 车况阈值告警（含本地预录音频）。等 Application/音频就绪后再起，
        // 因为其自检会真的播一次告警音。同样不阻塞构造函数。
        xTaskCreate(atz_alarm_boot_task, "atz_alarm_boot", 3072, NULL, 4, NULL);

        RaiseLvglTaskPriority();
        TuneLvglRefreshPeriod();
    }

    // 上游把 LVGL 渲染任务（esp_lvgl_port 的 "taskLVGL"）建在**最低优先级 1**，
    // 语音/AFE/解码那些任务都在 5+ —— 界面一动就被音频压住，动画自然卡。
    // 这里启动后把它抬到 ATZ_LVGL_TASK_PRIORITY（默认 3）：仍然低于音频任务，
    // 不会抢语音的 CPU，但渲染不再被饿死。
    void RaiseLvglTaskPriority() {
#if ATZ_LVGL_TASK_PRIORITY > 1
        TaskHandle_t lvgl_task = xTaskGetHandle("taskLVGL");
        if (lvgl_task == nullptr) {
            ESP_LOGW(TAG, "LVGL task not found, priority unchanged");
            return;
        }
        const UBaseType_t before = uxTaskPriorityGet(lvgl_task);
        vTaskPrioritySet(lvgl_task, ATZ_LVGL_TASK_PRIORITY);
        ESP_LOGI(TAG, "LVGL task priority %d -> %d (UI smoothness)", (int)before,
                 ATZ_LVGL_TASK_PRIORITY);
#endif
    }

    // LVGL 的刷新定时器周期默认由 Kconfig 定死（CONFIG_LV_DEF_REFR_PERIOD=33ms = 30fps 上限），
    // 动画最多只能跑到 ~22fps（实测）。这里用公开 API 在运行时把它改成
    // ATZ_LVGL_REFR_PERIOD_MS（默认 16 → 上限 ~60fps）。
    // 空屏时本来就不会出帧（实测 idle 0 帧），所以改小周期只是让"有动画时更顺"，
    // 不会凭空增加耗电。
    void TuneLvglRefreshPeriod() {
#if ATZ_LVGL_REFR_PERIOD_MS > 0
        lv_timer_t* refr = lv_display_get_refr_timer(lv_display_get_default());
        if (refr == nullptr) {
            ESP_LOGW(TAG, "LVGL refresh timer not found, period unchanged");
            return;
        }
        lv_timer_set_period(refr, ATZ_LVGL_REFR_PERIOD_MS);
        ESP_LOGI(TAG, "LVGL refresh period -> %u ms (UI frame rate)",
                 (unsigned)ATZ_LVGL_REFR_PERIOD_MS);
#endif
    }

    // 台架自检（public）：监听 seconds 秒，统计 I2C 读到多少次有效样本、最大手指数。
    // 用户点屏幕时用浏览器访问 /touch 就知道"面板到底答不答" ——
    // 这是区分"固件逻辑问题"与"面板不应答"的唯一可靠手段。
    bool TouchWatch(int seconds, char* out, size_t out_len) {
        if (seconds < 1) {
            seconds = 1;
        }
        if (seconds > 30) {
            seconds = 30;
        }
        if (!TouchAttached()) {
            TouchAttach(TP_PROBE_ATTEMPTS, true);   // 也许刚刚才恢复
        }
        if (!TouchAttached()) {
            snprintf(out, out_len,
                     "panel NOT RESPONDING on I2C (tried bus1 GPIO1/3 and bus0 TCA9554 bus). "
                     "No firmware change can help until the controller answers.");
            return false;
        }

        const int samples = seconds * (1000 / TP_POLL_INTERVAL_MS);
        int reads_ok = 0;
        int reads_fail = 0;
        int max_fingers = 0;
        for (int i = 0; i < samples; i++) {
            uint8_t data[5] = {};
            if (TouchRead(data, sizeof(data)) == ESP_OK) {
                reads_ok++;
                if (data[0] > max_fingers) {
                    max_fingers = data[0];
                }
            } else {
                reads_fail++;
            }
            vTaskDelay(pdMS_TO_TICKS(TP_POLL_INTERVAL_MS));
        }
        const char* verdict = max_fingers > 0  ? "TOUCH DETECTED"
                              : reads_ok > 0   ? "chip answering, but no touch seen"
                                               : "chip not answering";
        snprintf(out, out_len, "touch watch %ds: reads_ok=%d reads_fail=%d max_fingers=%d -> %s",
                 seconds, reads_ok, reads_fail, max_fingers, verdict);
        ESP_LOGI(TAG, "%s", out);
        return max_fingers > 0;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, I2S_STD_SLOT_BOTH, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN, I2S_STD_SLOT_RIGHT); // I2S_STD_SLOT_LEFT / I2S_STD_SLOT_RIGHT / I2S_STD_SLOT_BOTH

        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(AtzGaugeBoard);

AtzGaugeBoard* AtzGaugeBoard::instance_ = nullptr;

// ═══════════════════════════════════════════════════════════════════════════
// 给台架调试端点用的自由函数（见 atz_touch.h / atz_shot.cc 的 /touch）
// ═══════════════════════════════════════════════════════════════════════════

bool atz_touch_watch(int seconds, char* out, size_t out_len) {
    auto* board = static_cast<AtzGaugeBoard*>(&Board::GetInstance());
    if (board == nullptr || out == nullptr || out_len == 0) {
        return false;
    }
    return board->TouchWatch(seconds, out, out_len);
}

// Polls the CST816S and fires one "start conversation" per tap.
//
// Policy: only kDeviceStateIdle reacts. Every other state (connecting, listening,
// speaking, notifying, wifi-configuring, upgrading, ...) ignores the tap, so a
// stray touch while the assistant is answering can never cut it off.
//
// Runs on its own task and only touches thread-safe entry points:
// Application::ToggleChatState() just sets an event bit, which the main loop
// picks up -- no state is mutated from here.
void AtzGaugeBoard::TouchPollTask(void* arg) {
    auto* self = static_cast<AtzGaugeBoard*>(arg);
    auto& app = Application::GetInstance();

    int down_samples = 0;
    bool latched = false;        // one trigger per press: re-armed only after release
    uint32_t reads_ok = 0, reads_fail = 0, ticks = 0;
    uint8_t last_finger = 0;

    while (true) {
        uint8_t data[5] = {};
        esp_err_t err = self->TouchRead(data, sizeof(data));

        bool down = false;
        if (err == ESP_OK) {
            reads_ok++;
            last_finger = data[0];
            down = (data[0] > 0);   // data[0] = finger count
        } else {
            // A NACK is the normal idle case: a sleeping CST816S does not ACK its
            // address. Treat it as "not pressed" and keep polling -- never
            // ESP_ERROR_CHECK() here, that was the reboot loop.
            reads_fail++;
        }

        if (down) {
            if (!latched && ++down_samples >= TP_CONFIRM_SAMPLES) {
                latched = true;
                DeviceState state = app.GetDeviceState();
                if (state == kDeviceStateIdle) {
                    ESP_LOGI(TAG, "touch tap -> start conversation");
                    app.ToggleChatState();
                } else {
                    ESP_LOGD(TAG, "touch tap ignored in state %d", (int)state);
                }
            }
        } else {
            down_samples = 0;
            latched = false;
        }

        // Bring-up aid: one line every 5s telling whether the panel answers at all.
        // Without it a dead bus and an untouched-but-asleep panel look identical.
        // TP_DIAG=1 prints it at INFO; at 0 it stays DEBUG, i.e. invisible unless the
        // log level for this tag is raised.
        if (++ticks % (5000 / TP_POLL_INTERVAL_MS) == 0) {
            ESP_LOG_LEVEL(TP_DIAG ? ESP_LOG_INFO : ESP_LOG_DEBUG, TAG,
                          "touch diag: ok=%u fail=%u last_finger=%u",
                          (unsigned)reads_ok, (unsigned)reads_fail, (unsigned)last_finger);
        }

        vTaskDelay(pdMS_TO_TICKS(TP_POLL_INTERVAL_MS));
    }
}
