/*
 * ESP32-S3 AMOLED 2.06" watch — starter watch face.
 *
 * Shows a digital clock, date and a seconds ring on the 410x502 AMOLED.
 * Time starts from the firmware build time, then syncs over Wi-Fi (NTP).
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "wifi_time.h"
#include "battery.h"

static const char *TAG = "watch";

static lv_obj_t *s_wifi_label;
static lv_obj_t *s_batt_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_sec_label;
static lv_obj_t *s_date_label;
static lv_obj_t *s_sec_arc;

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

/* Runs inside the LVGL task, so the display lock is already held. */
static void clock_tick(lv_timer_t *timer)
{
    (void)timer;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    lv_label_set_text_fmt(s_time_label, "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text_fmt(s_sec_label, "%02d", tm.tm_sec);

    char date[40];
    strftime(date, sizeof(date), "%A, %d %B", &tm);
    lv_label_set_text(s_date_label, date);

    lv_arc_set_value(s_sec_arc, tm.tm_sec);

    /* Wi-Fi / time-sync status: grey = not set up, amber = working, green = synced, red = failed */
    uint32_t color;
    switch (wifi_time_state()) {
    case WIFI_TIME_CONNECTING:
    case WIFI_TIME_SYNCING: color = 0xFF9F0A; break;
    case WIFI_TIME_SYNCED:  color = 0x30D158; break;
    case WIFI_TIME_FAILED:  color = 0xFF453A; break;
    case WIFI_TIME_IDLE:
    case WIFI_TIME_NO_CONFIG:
    default:                color = 0x48484A; break;
    }
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(color), 0);
}

/* Battery: icon + percentage. Green while charging, red when low. */
static void battery_tick(lv_timer_t *timer)
{
    (void)timer;
    battery_info_t b = battery_read();

    if (!b.valid || !b.present || b.percent < 0) {
        lv_label_set_text(s_batt_label, b.usb_power ? LV_SYMBOL_USB : "");
        lv_obj_set_style_text_color(s_batt_label, lv_color_hex(0x8E8E93), 0);
        return;
    }

    const char *icon = b.percent > 80 ? LV_SYMBOL_BATTERY_FULL
                     : b.percent > 55 ? LV_SYMBOL_BATTERY_3
                     : b.percent > 30 ? LV_SYMBOL_BATTERY_2
                     : b.percent > 10 ? LV_SYMBOL_BATTERY_1
                     :                  LV_SYMBOL_BATTERY_EMPTY;
    if (b.charging) {
        lv_label_set_text_fmt(s_batt_label, LV_SYMBOL_CHARGE " %s %d%%", icon, b.percent);
    } else {
        lv_label_set_text_fmt(s_batt_label, "%s %d%%", icon, b.percent);
    }

    uint32_t color = b.charging || b.full ? 0x30D158   /* green */
                   : b.percent <= 15      ? 0xFF453A   /* red */
                   :                        0xD1D1D6;  /* light grey */
    lv_obj_set_style_text_color(s_batt_label, lv_color_hex(color), 0);
}

static void build_watch_face(void)
{
    lv_obj_t *scr = lv_screen_active();
    /* Pure black = pixels off on AMOLED: looks best and saves battery. */
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Seconds ring */
    s_sec_arc = lv_arc_create(scr);
    lv_obj_set_size(s_sec_arc, 360, 360);
    lv_obj_center(s_sec_arc);
    lv_arc_set_rotation(s_sec_arc, 270);
    lv_arc_set_bg_angles(s_sec_arc, 0, 360);
    lv_arc_set_range(s_sec_arc, 0, 59);
    lv_obj_remove_style(s_sec_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_sec_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_sec_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_sec_arc, lv_color_hex(0x1C1C1E), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_sec_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_sec_arc, lv_color_hex(0x30D158), LV_PART_INDICATOR);

    /* HH:MM */
    s_time_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_white(), 0);
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -10);

    /* Seconds */
    s_sec_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_sec_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_sec_label, lv_color_hex(0x30D158), 0);
    lv_obj_align(s_sec_label, LV_ALIGN_CENTER, 0, 45);

    /* Date */
    s_date_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0x8E8E93), 0);
    lv_obj_align(s_date_label, LV_ALIGN_TOP_MID, 0, 30);

    /* Sync status icon */
    s_wifi_label = lv_label_create(scr);
    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_wifi_label, LV_ALIGN_BOTTOM_MID, -60, -30);

    /* Battery, beside the Wi-Fi icon */
    s_batt_label = lv_label_create(scr);
    lv_label_set_text(s_batt_label, "");
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_batt_label, LV_ALIGN_BOTTOM_MID, 25, -30);

    clock_tick(NULL);
    lv_timer_create(clock_tick, 250, NULL);
    battery_tick(NULL);
    lv_timer_create(battery_tick, 5000, NULL);
}

void app_main(void)
{
    setenv("TZ", CONFIG_WATCH_TIMEZONE, 1);
    tzset();
    seed_time_from_build();

    bsp_display_start(); /* also brings up the shared I2C bus */

    if (battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery monitor unavailable");
    }

    bsp_display_lock(0);
    build_watch_face();
    bsp_display_unlock();

    ESP_LOGI(TAG, "watch face running");

    wifi_time_start();
}
