// components/bsp/src/bsp_battery.c
// 移植自 trae_card/components/platform/platform_esp32/src/battery_cw2017.c
// (去掉了电池 profile 写入部分:开源硬件用户电池各异,用芯片自带 Li-Poly profile 更通用)
#include "bsp_battery.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

static const char *TAG = "bsp_batt";

#define CW_REG_VERSION   0x00   // 版本号,上电应答即代表芯片在位
#define CW_REG_VCELL_H   0x02   // 14bit 电压,V(uV) = raw * 312.5
#define CW_REG_SOC_H     0x04   // 高字节 = 整数百分比;低字节(0x05)= 1/256 %
#define CW_REG_CONFIG    0x08   // 0xF0=睡眠 / 0x30=复位态 / 0x00=正常

static i2c_master_dev_handle_t s_dev;
static int s_last_soc = -1;
static bool s_soc_estimated;

typedef struct {
    uint16_t millivolts;
    uint8_t percent;
} battery_curve_point_t;

static int estimate_soc_from_voltage(int millivolts) {
    // Conservative single-cell Li-polymer resting-voltage curve. Linear
    // interpolation keeps the displayed value stable instead of jumping
    // between broad fixed bands when the gauge's SOC register is uncalibrated.
    static const battery_curve_point_t curve[] = {
        {3300, 0}, {3500, 5}, {3650, 10}, {3680, 15}, {3700, 20},
        {3720, 25}, {3740, 30}, {3760, 35}, {3780, 40}, {3800, 45},
        {3820, 50}, {3850, 55}, {3870, 60}, {3910, 65}, {3950, 70},
        {3980, 75}, {4020, 80}, {4080, 85}, {4110, 90}, {4150, 95},
        {4200, 100},
    };
    if (millivolts <= curve[0].millivolts) return curve[0].percent;
    size_t count = sizeof(curve) / sizeof(curve[0]);
    if (millivolts >= curve[count - 1].millivolts) return curve[count - 1].percent;
    for (size_t i = 1; i < count; ++i) {
        if (millivolts <= curve[i].millivolts) {
            int span_mv = curve[i].millivolts - curve[i - 1].millivolts;
            int span_percent = curve[i].percent - curve[i - 1].percent;
            return curve[i - 1].percent +
                   (millivolts - curve[i - 1].millivolts) * span_percent / span_mv;
        }
    }
    return -1;
}

static int cw_read(uint8_t reg, uint8_t *buf, size_t n) {
    if (!s_dev) return -1;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100) == ESP_OK ? 0 : -1;
}

static int cw_write(uint8_t reg, uint8_t val) {
    if (!s_dev) return -1;
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100) == ESP_OK ? 0 : -1;
}

esp_err_t bsp_battery_init(void) {
    if (s_dev) return ESP_OK;

    esp_err_t e = bsp_i2c_init();
    if (e != ESP_OK) return e;

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BSP_I2C_CW2017_ADDR,
        .scl_speed_hz    = 100000,
    };
    e = i2c_master_bus_add_device(bsp_i2c_bus(), &dc, &s_dev);
    if (e != ESP_OK) { ESP_LOGE(TAG, "添加 I2C 设备失败: %s", esp_err_to_name(e)); return e; }

    uint8_t ver = 0;
    int version_read = -1;
    for (int attempt = 0; attempt < 3 && version_read != 0; ++attempt) {
        version_read = cw_read(CW_REG_VERSION, &ver, 1);
        if (version_read != 0) {
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    if (version_read != 0) {
        ESP_LOGW(TAG, "CW2017 未应答 —— 用 bsp_i2c_scan() 确认 0x%02X 是否在线;"
                      "无电量计的板子可忽略本项", BSP_I2C_CW2017_ADDR);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "检测到 CW2017 VERSION=0x%02X", ver);

    // 确保处于正常工作模式(非睡眠/复位态)。用芯片自带 Li-Poly profile,不写自定义 profile。
    cw_write(CW_REG_CONFIG, 0x00);
    vTaskDelay(pdMS_TO_TICKS(100));   // 等首次 SOC 计算完成

    return ESP_OK;
}

int bsp_battery_soc(void) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint8_t b[2] = {0};
        if (cw_read(CW_REG_SOC_H, b, 2) == 0) {
            int soc = b[0];               // 高字节即整数百分比
            if (soc <= 100) {
                s_last_soc = soc;
                s_soc_estimated = false;
                return soc;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Some units answer at the expected address and provide a valid voltage,
    // but their SOC register remains uncalibrated (commonly 0xFFFF). Use the
    // measured voltage as a transparent fallback so settings never go blank.
    int millivolts = bsp_battery_mv();
    int estimated = estimate_soc_from_voltage(millivolts);
    if (estimated >= 0) {
        s_last_soc = estimated;
        s_soc_estimated = true;
    }
    return s_last_soc;
}

bool bsp_battery_soc_is_estimated(void) {
    return s_soc_estimated;
}

int bsp_battery_mv(void) {
    uint8_t b[2] = { 0 };
    if (cw_read(CW_REG_VCELL_H, b, 2) != 0) return -1;
    uint32_t raw = ((uint32_t)b[0] << 8 | b[1]) & 0x3FFF;   // 14bit
    return (int)((raw * 3125) / 10000);                     // raw * 312.5uV → mV
}
