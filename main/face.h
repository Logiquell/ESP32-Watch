#pragma once

#include <stdbool.h>
#include <time.h>
#include "battery.h"
#include "wifi_time.h"

/*
 * Watch face interface. One face is compiled in, picked in menuconfig → Watch → Watch face:
 *   face_flip.c - vintage flip clock with radium-glow numerals (default)
 *   face_lcd.c  - 80s digital LCD
 * All functions must run with the LVGL lock held (e.g. from an lv_timer).
 * Updates only touch the screen when something actually changed.
 */
void face_create(void);

/* colon_on: 1 Hz blink phase (used by faces that blink a colon). */
void face_set_time(const struct tm *tm, bool colon_on);

/* blink: shared blink phase, used for low battery. */
void face_set_battery(const battery_info_t *b, bool blink);

/* blink: shared blink phase, used while syncing. */
void face_set_sync(wifi_time_state_t state, bool blink);

/* Show "LO" (critical battery, about to power off). */
void face_show_low_battery(void);

/* Next face_set_time() jumps straight to the time without animating (e.g. on screen wake). */
void face_skip_animation(void);
