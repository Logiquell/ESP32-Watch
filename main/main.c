/*
 * ESP32-S3 AMOLED 2.06" watch.
 *
 * Watch face picked in menuconfig: vintage flip clock (face_flip.c, default) or
 * 80s digital LCD (face_lcd.c).
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
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "wifi_time.h"
#include "battery.h"
#include "screen.h"
#include "rtc.h"
#include "face.h"

static const char *TAG = "watch";

/*
 * Restart diagnostics that survive resets (but not a full power-on): counts restarts and
 * remembers why the last one happened. Printed every 30 s, so after testing on battery you
 * can plug in USB and read what happened while it was unplugged.
 */
#define DIAG_MAGIC 0x57A7C4D5
static RTC_NOINIT_ATTR uint32_t s_diag_magic;
static RTC_NOINIT_ATTR uint32_t s_diag_restarts;
static RTC_NOINIT_ATTR uint32_t s_diag_last_reason;

static battery_info_t s_battery = {.percent = -1};
static int s_critical_count;     /* consecutive critical readings */
static bool s_shutting_down;

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

/* Set the system clock at boot: the RTC if it holds a valid time, else the build time.
   Returns true if the time came from the RTC (i.e. it's trustworthy). */
static bool seed_time(void)
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
        return true;
    }
    seed_time_from_build();
    ESP_LOGI(TAG, "RTC time not available, using build time until Wi-Fi sync");
    return false;
}

/* Runs inside the LVGL task every 250 ms, so the display lock is already held. */
static void face_tick(lv_timer_t *timer)
{
    if (s_shutting_down || (timer && !screen_is_awake())) {
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

static void power_off_cb(lv_timer_t *timer)
{
    (void)timer;
    battery_power_off();
}

/*
 * Every 5 s, screen on or off: read the PMU (I2C) into the cache the face draws from,
 * and switch the watch off cleanly if the battery is critical on two readings in a row
 * (one reading can dip while the screen wakes). Running the battery flat instead makes
 * the chip brown out and reset every time the screen tries to wake.
 */
static const char *reset_reason_name(esp_reset_reason_t r);

/*
 * Drain measurement for the most recent stretch on battery: % at unplug, % now, and how
 * long. Kept after USB is plugged back in, so it can be read over the serial log.
 */
static int s_run_start_pct = -1, s_run_last_pct = -1;
static int64_t s_run_start_us, s_run_last_us;
static bool s_on_battery;

static void track_battery_run(const battery_info_t *b)
{
    if (!b->valid || b->percent < 0) {
        return;
    }
    int64_t now = esp_timer_get_time();
    bool on_batt = !b->usb_power;
    if (on_batt && !s_on_battery) { /* just unplugged: start a new run */
        s_run_start_pct = b->percent;
        s_run_start_us = now;
    }
    if (on_batt) {
        s_run_last_pct = b->percent;
        s_run_last_us = now;
    }
    s_on_battery = on_batt;
}

/*
 * Snapshot of the stats above, kept in RTC memory so it survives a restart. USB serial
 * stops working after light sleep, so after a test on battery: plug in, restart the watch,
 * and the boot log prints what happened before the restart.
 */
#define SAVED_MAGIC 0x5AFE57A7
typedef struct {
    uint32_t magic;
    int16_t run_start_pct, run_last_pct;
    uint32_t run_minutes;
    wifi_time_stats_t wifi;
    char pm[1200]; /* esp_pm_dump_locks() text */
} saved_diag_t;
static RTC_NOINIT_ATTR saved_diag_t s_saved;

static void save_diagnostics(bool with_pm)
{
    s_saved.magic = SAVED_MAGIC;
    s_saved.run_start_pct = (int16_t)s_run_start_pct;
    s_saved.run_last_pct = (int16_t)s_run_last_pct;
    s_saved.run_minutes = (uint32_t)((s_run_last_us - s_run_start_us) / 60000000);
    s_saved.wifi = wifi_time_stats();
#if CONFIG_PM_PROFILING
    if (with_pm) {
        FILE *f = fmemopen(s_saved.pm, sizeof(s_saved.pm) - 1, "w");
        if (f) {
            esp_pm_dump_locks(f);
            fclose(f);
        }
        s_saved.pm[sizeof(s_saved.pm) - 1] = '\0';
    }
#endif
}

static void print_saved_diagnostics(esp_reset_reason_t reason)
{
    if (s_saved.magic != SAVED_MAGIC || reason == ESP_RST_POWERON) {
        return;
    }
    ESP_LOGI(TAG, "saved diag from before restart:");
    if (s_saved.run_start_pct >= 0) {
        ESP_LOGI(TAG, "  battery run: %d%% -> %d%% over %lu min", s_saved.run_start_pct,
                 s_saved.run_last_pct, (unsigned long)s_saved.run_minutes);
    }
    ESP_LOGI(TAG, "  wifi: %lu syncs, %lu failed, radio on %lu s, last disconnect reason %u",
             (unsigned long)s_saved.wifi.attempts, (unsigned long)s_saved.wifi.failures,
             (unsigned long)(s_saved.wifi.radio_on_ms / 1000), s_saved.wifi.last_disconnect_reason);
    s_saved.pm[sizeof(s_saved.pm) - 1] = '\0';
    printf("%s\n", s_saved.pm);
    s_saved.magic = 0; /* print once */
}

static void log_diagnostics(void)
{
    ESP_LOGI(TAG, "diag: restarts since power-on=%lu, last restart=%s, uptime=%llds",
             (unsigned long)s_diag_restarts, reset_reason_name((esp_reset_reason_t)s_diag_last_reason),
             esp_timer_get_time() / 1000000);
    if (s_run_start_pct >= 0) {
        int minutes = (int)((s_run_last_us - s_run_start_us) / 60000000);
        int used = s_run_start_pct - s_run_last_pct;
        ESP_LOGI(TAG, "diag: %s battery run: %d%% -> %d%% over %d min (%.1f %%/h)",
                 s_on_battery ? "current" : "last", s_run_start_pct, s_run_last_pct, minutes,
                 minutes > 0 ? used * 60.0f / minutes : 0.0f);
    }
#if CONFIG_PM_PROFILING
    esp_pm_dump_locks(stdout); /* includes time spent in each power mode since boot */
#endif
}

static void battery_tick(lv_timer_t *timer)
{
    static int ticks;
    bool minute = timer && ++ticks % 12 == 0;
    if (minute) { /* every 60 s */
        log_diagnostics();
    }
    save_diagnostics(minute);
    if (s_shutting_down) {
        return;
    }
    s_battery = battery_read();
    track_battery_run(&s_battery);
    s_critical_count = battery_is_critical(&s_battery) ? s_critical_count + 1 : 0;
    if (s_critical_count < 2) {
        return;
    }
    s_shutting_down = true;
    ESP_LOGW(TAG, "battery critical (%d%%, %u mV)", s_battery.percent, s_battery.millivolts);
    if (screen_is_awake()) {
        face_show_low_battery();
        lv_timer_t *t = lv_timer_create(power_off_cb, 2000, NULL); /* let "LO" show briefly */
        lv_timer_set_repeat_count(t, 1);
    } else {
        battery_power_off(); /* don't light the screen on a nearly empty battery */
    }
}

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (supply voltage dropped)";
    case ESP_RST_USB:      return "USB";
    case ESP_RST_EXT:      return "external pin";
    default:               return "other";
    }
}

/* Called by the screen module right before it lights up again. */
static void on_screen_wake(void)
{
    face_skip_animation(); /* jump straight to the current time, no flip while lighting up */
    battery_tick(NULL);
    face_tick(NULL);
}

/*
 * LVGL's clock. The LVGL port normally advances it from a periodic 5 ms esp_timer, which
 * wakes the CPU 200 times a second and all but prevents light sleep. Instead, LVGL reads
 * the time straight from esp_timer (stays correct across light sleep), and the port's
 * tick timer is stopped.
 */
static uint32_t lvgl_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void use_tickless_lvgl_clock(void)
{
    bsp_display_lock(0);
    lv_tick_set_cb(lvgl_tick_ms);
    bsp_display_unlock();
    lvgl_port_stop();       /* stops the 5 ms tick timer... but also pauses LVGL timers, */
    bsp_display_lock(0);
    lv_timer_enable(true);  /* ...so switch LVGL's own timers straight back on */
    bsp_display_unlock();
}

void app_main(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON || s_diag_magic != DIAG_MAGIC) {
        s_diag_magic = DIAG_MAGIC;
        s_diag_restarts = 0;
    } else {
        s_diag_restarts++;
    }
    s_diag_last_reason = reason;
    ESP_LOGI(TAG, "reset reason: %s (restarts since power-on: %lu)", reset_reason_name(reason),
             (unsigned long)s_diag_restarts);
    print_saved_diagnostics(reason);

    /* Check the battery before lighting the display: on a nearly flat battery, switch
       straight off instead of browning out in a reset loop. */
    if (battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery monitor unavailable");
    } else {
        battery_info_t b = battery_read();
        if (battery_is_critical(&b)) {
            ESP_LOGW(TAG, "battery critical at boot (%d%%, %u mV)", b.percent, b.millivolts);
            vTaskDelay(pdMS_TO_TICKS(100)); /* let the log get out */
            battery_power_off();
            vTaskDelay(portMAX_DELAY);
        }
    }

    setenv("TZ", CONFIG_WATCH_TIMEZONE, 1);
    tzset();
    bool time_known = seed_time();

    /*
     * The BSP's LVGL draw buffer lives in PSRAM, so the SPI driver copies each flush into
     * a temporary internal DMA buffer of the same size. sdkconfig.defaults sets
     * CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=20 (16 KB per flush instead of 82 KB) so that
     * copy always fits; at 100 lines it sometimes didn't ("spi transmit (queue) color
     * failed"), leaving parts of the screen undrawn.
     */
    bsp_display_start(); /* also brings up the shared I2C bus */
    use_tickless_lvgl_clock();
    ESP_LOGI(TAG, "internal DMA RAM: %u free, largest block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

    bsp_display_lock(0);
    face_create();
    battery_tick(NULL);
    face_tick(NULL);
    lv_timer_create(face_tick, 250, NULL);
    lv_timer_create(battery_tick, 5000, NULL);
    screen_init(on_screen_wake);
    bsp_display_unlock();

    ESP_LOGI(TAG, "watch face running");

    wifi_time_start(time_known);
}
