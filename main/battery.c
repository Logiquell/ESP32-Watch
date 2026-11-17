/*
 * Minimal AXP2101 PMU reader: battery %, voltage, charging and USB status.
 * Register map from the AXP2101 datasheet (same layout XPowersLib uses).
 */
#include "battery.h"

#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "battery";

#define AXP2101_ADDR        0x34
#define REG_STATUS1         0x00  /* bit5 VBUS good, bit3 battery present */
#define REG_STATUS2         0x01  /* bits6:5 current dir (01 = charging), bits2:0 charge state (4 = done) */
#define REG_GAUGE_CTRL      0x18  /* bit3 fuel gauge enable */
#define REG_ADC_CTRL        0x30  /* bit0 battery voltage ADC enable */
#define REG_VBAT_H          0x34  /* bits5:0 = VBAT[13:8], in mV */
#define REG_VBAT_L          0x35
#define REG_BAT_PERCENT     0xA4  /* fuel gauge result, 0-100 */

#define I2C_TIMEOUT_MS      50

static i2c_master_dev_handle_t s_dev;

static esp_err_t reg_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, I2C_TIMEOUT_MS);
}

static esp_err_t reg_set_bits(uint8_t reg, uint8_t mask)
{
    uint8_t v;
    esp_err_t err = reg_read(reg, &v, 1);
    if (err != ESP_OK) {
        return err;
    }
    if ((v & mask) == mask) {
        return ESP_OK;
    }
    uint8_t buf[2] = {reg, (uint8_t)(v | mask)};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t battery_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), TAG, "add AXP2101");

    /* Make sure the fuel gauge and battery-voltage ADC are running. */
    ESP_RETURN_ON_ERROR(reg_set_bits(REG_GAUGE_CTRL, 1 << 3), TAG, "enable gauge");
    ESP_RETURN_ON_ERROR(reg_set_bits(REG_ADC_CTRL, 1 << 0), TAG, "enable VBAT ADC");

    battery_info_t b = battery_read();
    uint8_t s1 = 0, s2 = 0;
    reg_read(REG_STATUS1, &s1, 1);
    reg_read(REG_STATUS2, &s2, 1);
    ESP_LOGI(TAG, "AXP2101: status1=0x%02x status2=0x%02x -> present=%d usb=%d charging=%d full=%d %d%% %u mV",
             s1, s2, b.present, b.usb_power, b.charging, b.full, b.percent, b.millivolts);
    return ESP_OK;
}

battery_info_t battery_read(void)
{
    battery_info_t b = {.percent = -1};
    if (s_dev == NULL) {
        return b;
    }
    uint8_t s1, s2, vbat[2], pct;
    if (reg_read(REG_STATUS1, &s1, 1) != ESP_OK ||
        reg_read(REG_STATUS2, &s2, 1) != ESP_OK ||
        reg_read(REG_VBAT_H, vbat, 2) != ESP_OK ||
        reg_read(REG_BAT_PERCENT, &pct, 1) != ESP_OK) {
        return b;
    }
    b.valid      = true;
    b.usb_power  = s1 & (1 << 5);
    b.present    = s1 & (1 << 3);
    b.charging   = ((s2 >> 5) & 0x3) == 0x1;
    b.full       = (s2 & 0x7) == 0x4;
    b.millivolts = ((vbat[0] & 0x3F) << 8) | vbat[1];
    b.percent    = (b.present && pct <= 100) ? pct : -1;
    return b;
}
