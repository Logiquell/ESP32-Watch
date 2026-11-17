/*
 * Minimal PCF85063 RTC driver. Time is kept in UTC; the timezone is applied
 * by the C library (TZ), so DST changes never need an RTC write.
 * Register map: PCF85063A datasheet, section 8.
 */
#include "rtc.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "rtc";

#define PCF85063_ADDR   0x51
#define REG_CONTROL_1   0x00  /* bit1 = 12h mode (we want 0 = 24h) */
#define REG_SECONDS     0x04  /* bit7 = OS: oscillator stopped, time not reliable */
/* 0x04 seconds, 0x05 minutes, 0x06 hours, 0x07 day, 0x08 weekday, 0x09 month, 0x0A year */

#define I2C_TIMEOUT_MS  50

static i2c_master_dev_handle_t s_dev;

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* UTC broken-down time -> time_t without touching the TZ environment. */
static time_t utc_mktime(const struct tm *tm)
{
    /* days since 1970-01-01, from Howard Hinnant's days_from_civil */
    int y = tm->tm_year + 1900;
    unsigned m = tm->tm_mon + 1, d = tm->tm_mday;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned mp = m > 2 ? m - 3 : m + 9; /* March-based month */
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097L + (long)doe - 719468L;
    return (time_t)days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

esp_err_t ext_rtc_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, TAG, "no I2C bus");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, PCF85063_ADDR, I2C_TIMEOUT_MS), TAG,
                        "PCF85063 not found at 0x%02x", PCF85063_ADDR);

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add device");

    /* Make sure the hours register is in 24-hour mode. */
    uint8_t reg = REG_CONTROL_1, ctrl1;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_dev, &reg, 1, &ctrl1, 1, I2C_TIMEOUT_MS), TAG, "read ctrl1");
    if (ctrl1 & (1 << 1)) {
        uint8_t buf[2] = {REG_CONTROL_1, (uint8_t)(ctrl1 & ~(1 << 1))};
        ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, buf, 2, I2C_TIMEOUT_MS), TAG, "set 24h");
    }
    return ESP_OK;
}

esp_err_t ext_rtc_read(time_t *out)
{
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    uint8_t reg = REG_SECONDS, r[7];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_dev, &reg, 1, r, sizeof(r), I2C_TIMEOUT_MS), TAG, "read time");

    if (r[0] & 0x80) {
        ESP_LOGW(TAG, "oscillator-stop flag set: RTC lost power, time not valid");
        return ESP_ERR_INVALID_STATE;
    }
    struct tm tm = {
        .tm_sec  = bcd2bin(r[0] & 0x7F),
        .tm_min  = bcd2bin(r[1] & 0x7F),
        .tm_hour = bcd2bin(r[2] & 0x3F),
        .tm_mday = bcd2bin(r[3] & 0x3F),
        .tm_mon  = bcd2bin(r[5] & 0x1F) - 1,
        .tm_year = bcd2bin(r[6]) + 100, /* years since 1900; chip stores 00-99 = 2000-2099 */
    };
    if (tm.tm_year < 124 || tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 ||
        tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 59) {
        ESP_LOGW(TAG, "implausible RTC date %04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        return ESP_ERR_INVALID_STATE;
    }
    *out = utc_mktime(&tm);
    return ESP_OK;
}

esp_err_t ext_rtc_write(time_t t)
{
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    struct tm tm;
    gmtime_r(&t, &tm);
    uint8_t buf[8] = {
        REG_SECONDS,
        bin2bcd(tm.tm_sec),          /* bit7 = 0 clears the oscillator-stop flag */
        bin2bcd(tm.tm_min),
        bin2bcd(tm.tm_hour),
        bin2bcd(tm.tm_mday),
        (uint8_t)tm.tm_wday,
        bin2bcd(tm.tm_mon + 1),
        bin2bcd(tm.tm_year % 100),
    };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}
