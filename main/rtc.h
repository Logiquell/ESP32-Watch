#pragma once

#include <time.h>
#include "esp_err.h"

/* Attach to the PCF85063 real-time clock on the board's shared I2C bus. */
esp_err_t ext_rtc_init(void);

/*
 * Read the RTC (stored as UTC).
 * Returns ESP_ERR_INVALID_STATE if the chip reports its oscillator stopped
 * (power was lost) or the stored date is implausible.
 */
esp_err_t ext_rtc_read(time_t *out);

/* Write a UTC time to the RTC (also clears the oscillator-stop flag). */
esp_err_t ext_rtc_write(time_t t);
