/*
 * ESP32-S3 AMOLED 2.06" watch.
 *
 * 80s digital-LCD watch face (face_lcd.c): time, seconds, date, weekday, battery
 * bars and sync status.
 * Time comes from the PCF85063 RTC at boot (or the build time if the RTC lost
 * power), then syncs over Wi-Fi (NTP), which also updates the RTC.
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "wifi_time.h"
#include "battery.h"
#include "screen.h"
#include "rtc.h"
#include "face_lcd.h"

static const char *TAG = "watch";

static battery_info_t s_battery = {.percent = -1};

/* Seed the system clock from the compile timestamp so the face shows something sensible. */
static void seed_time_from_build(void)
{
    struct tm tm = {0};
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %s", __DATE__, __TIME__); /* "Nov 17 2026 14:03:22" */
    if (strptime(buf, "%b %d %Y %H:%M:%S", &tm) == NULL) {
        ESP_LOGW(TAG, "could not parse build time '%s'", buf);
        return;
    }
    struct timeval tv = {.tv_sec = mktime(&tm), .tv_usec = 0};
    settimeofday(&tv, NULL);
}

/* Set the system clock at boot: the RTC if it holds a valid time, else the build time. */
static void seed_time(void)
{
    time_t t;
    if (ext_rtc_init() == ESP_OK && ext_rtc_read(&t) == ESP_OK) {
        struct timeval tv = {.tv_sec = t, .tv_usec = 0};
        settimeofday(&tv, NULL);
        struct tm tm;
        char buf[32];
        localtime_r(&t, &tm);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        ESP_LOGI(TAG, "time from RTC: %s", buf);
    } else {
        seed_time_from_build();
        ESP_LOGI(TAG, "RTC time not available, using build time until Wi-Fi sync");
    }
}

/* Runs inside the LVGL task every 250 ms, so the display lock is already held. */
static void face_tick(lv_timer_t *timer)
{
    if (timer && !screen_is_awake()) {
        return; /* no redraws while dark: saves CPU and QSPI traffic */
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);

    bool first_half = tv.tv_usec < 500000; /* 1 Hz blink phase */
    face_set_time(&tm, first_half);
    face_set_sync(wifi_time_state(), first_half);
    face_set_battery(&s_battery, first_half);
}

/* Every 5 s: read the PMU (I2C) into the cache the face draws from. */
static void battery_tick(lv_timer_t *timer)
{
    if (timer && !screen_is_awake()) {
        return;
    }
    s_battery = battery_read();
}

/* Called by the screen module right before it lights up again. */
static void on_screen_wake(void)
{
    battery_tick(NULL);
    face_tick(NULL);
}

void app_main(void)
{
    setenv("TZ", CONFIG_WATCH_TIMEZONE, 1);
    tzset();
    seed_time();

    /*
     * The BSP's LVGL draw buffer lives in PSRAM, so the SPI driver copies each flush into
     * a temporary internal DMA buffer of the same size. sdkconfig.defaults sets
     * CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=20 (16 KB per flush instead of 82 KB) so that
     * copy always fits; at 100 lines it sometimes didn't ("spi transmit (queue) color
     * failed"), leaving parts of the screen undrawn.
     */
    bsp_display_start(); /* also brings up the shared I2C bus */
    ESP_LOGI(TAG, "internal DMA RAM: %u free, largest block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

    if (battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery monitor unavailable");
    }

    bsp_display_lock(0);
    face_create();
    battery_tick(NULL);
    face_tick(NULL);
    lv_timer_create(face_tick, 250, NULL);
    lv_timer_create(battery_tick, 5000, NULL);
    screen_init(on_screen_wake);
    bsp_display_unlock();

    ESP_LOGI(TAG, "watch face running");

    wifi_time_start();
}
