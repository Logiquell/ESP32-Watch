#pragma once

#include <stdbool.h>

/*
 * Screen sleep / wake.
 *  - Fades the AMOLED off after CONFIG_WATCH_SCREEN_TIMEOUT_S without touch.
 *  - Wakes on touch, or on the BOOT button (GPIO0), which also puts it to sleep.
 * Call after bsp_display_start(), with the display lock held.
 * on_wake runs in the LVGL task right as the screen turns back on (may be NULL).
 */
void screen_init(void (*on_wake)(void));

bool screen_is_awake(void);
