#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool     valid;        /* false if the PMU could not be read */
    bool     present;      /* battery connected */
    bool     usb_power;    /* USB / VBUS power present */
    bool     charging;     /* actively charging */
    bool     full;         /* charge complete */
    int      percent;      /* 0-100 from the AXP2101 fuel gauge, -1 if unknown */
    uint16_t millivolts;   /* battery voltage */
} battery_info_t;

/* Attach to the AXP2101 on the board's shared I2C bus and enable the fuel gauge + ADC. */
esp_err_t battery_init(void);

/* Read the current battery state (one short I2C transaction burst). */
battery_info_t battery_read(void);

/* True when running on battery (no USB) and it's critically low. */
bool battery_is_critical(const battery_info_t *b);

/* Ask the AXP2101 to switch the whole watch off. Press PWR or plug in USB to turn it back on. */
void battery_power_off(void);
