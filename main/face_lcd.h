#pragma once

#include <stdbool.h>
#include <time.h>
#include "battery.h"
#include "wifi_time.h"

/*
 * 80s digital-LCD watch face: seven-segment digits with faint "ghost" segments,
 * grey-green LCD panel inside a black bezel.
 * All functions must run with the LVGL lock held (e.g. from an lv_timer).
 * Updates only touch the screen when something actually changed.
 */
void face_create(void);

/* colon_on: blink phase for the HH:MM colon. */
void face_set_time(const struct tm *tm, bool colon_on);

/* blink: shared blink phase, used for low battery. */
void face_set_battery(const battery_info_t *b, bool blink);

/* blink: shared blink phase, used while syncing. */
void face_set_sync(wifi_time_state_t state, bool blink);
