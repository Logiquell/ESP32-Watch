#include "wifi_time.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "rtc.h"
#include "battery.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#define HAVE_WIFI_SECRETS 1
#else
#define HAVE_WIFI_SECRETS 0
#endif

static const char *TAG = "wifi_time";

#define CONNECT_TIMEOUT_MS   30000
#define SNTP_TIMEOUT_MS      15000
#define RESYNC_INTERVAL_MS   (CONFIG_WATCH_RESYNC_HOURS * 60 * 60 * 1000)
#define RETRY_INTERVAL_MS    (5 * 60 * 1000)

#define BIT_GOT_IP BIT0

/*
 * Long delays must not go through pdMS_TO_TICKS: it multiplies in 32 bits, so 6 h
 * (21,600,000 ms x 1000 Hz) wrapped around to ~125 s and Wi-Fi synced every 2 minutes,
 * draining the battery. Convert in 64 bits instead.
 */
static TickType_t ms_to_ticks(uint32_t ms)
{
    return (TickType_t)(((uint64_t)ms * configTICK_RATE_HZ) / 1000U);
}

static EventGroupHandle_t s_events;
static volatile wifi_time_state_t s_state = WIFI_TIME_NO_CONFIG;
static volatile time_t s_last_sync;
static volatile bool s_want_connection;
static wifi_time_stats_t s_stats;

wifi_time_state_t wifi_time_state(void) { return s_state; }
time_t wifi_time_last_sync(void) { return s_last_sync; }
wifi_time_stats_t wifi_time_stats(void) { return s_stats; }

#if HAVE_WIFI_SECRETS

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        static uint8_t last_reason;
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        /* log each new reason once (not every retry), and skip our own deliberate stop */
        if (s_want_connection && ev->reason != last_reason) {
            /* 201 = network not found (wrong name / 5 GHz only), 15 or 204 = wrong password,
               210/211 = security mode mismatch */
            ESP_LOGW(TAG, "disconnected, reason %d", ev->reason);
            last_reason = ev->reason;
        }
        if (s_want_connection) {
            s_stats.last_disconnect_reason = ev->reason;
        }
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        if (s_want_connection) {
            esp_wifi_connect(); /* keep trying until the sync task gives up */
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

static bool sync_once(void)
{
    s_state = WIFI_TIME_CONNECTING;
    s_want_connection = true;
    s_stats.attempts++;
    int64_t radio_on_us = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    bool ok = false;
    if (bits & BIT_GOT_IP) {
        s_state = WIFI_TIME_SYNCING;
        esp_netif_sntp_start(); /* (re)start: sends a request right away */
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_TIMEOUT_MS)) == ESP_OK) {
            ok = true;
        } else {
            ESP_LOGW(TAG, "no NTP reply within %d ms", SNTP_TIMEOUT_MS);
        }
    } else {
        ESP_LOGW(TAG, "could not join \"%s\" within %d ms", WIFI_SSID, CONNECT_TIMEOUT_MS);
    }

    s_want_connection = false;
    esp_wifi_stop(); /* radio off between syncs: big battery saving */
    xEventGroupClearBits(s_events, BIT_GOT_IP);
    s_stats.radio_on_ms += (uint32_t)((esp_timer_get_time() - radio_on_us) / 1000);
    if (!ok) {
        s_stats.failures++;
    }
    return ok;
}

/*
 * When to sync: the RTC keeps time well for a day or more, and the watch is charged daily,
 * so Wi-Fi only runs while USB is connected: once soon after plugging in, then every
 * CONFIG_WATCH_RESYNC_HOURS while it stays plugged in. On battery the radio stays off,
 * unless the watch has no valid time at all (RTC lost power), then it retries every 30 min.
 */
#define CHECK_INTERVAL_MS        (60 * 1000)
#define RETRY_ON_BATTERY_MS      (30 * 60 * 1000)

static bool s_time_known;

static void handle_sync_result(bool ok)
{
    if (!ok) {
        s_state = WIFI_TIME_FAILED;
        return;
    }
    s_last_sync = time(NULL);
    s_time_known = true;
    s_state = WIFI_TIME_SYNCED;
    struct tm tm;
    char buf[32];
    localtime_r((const time_t *)&s_last_sync, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    ESP_LOGI(TAG, "time synced: %s (%s)", buf, CONFIG_WATCH_TIMEZONE);
    if (ext_rtc_write(s_last_sync) == ESP_OK) {
        ESP_LOGI(TAG, "saved to RTC");
    } else {
        ESP_LOGW(TAG, "could not save time to RTC");
    }
}

static void sync_task(void *arg)
{
    bool was_on_usb = false;
    bool synced_this_charge = false;
    int64_t last_attempt_us = 0;
    bool attempted = false;

    for (;;) {
        battery_info_t b = battery_read();
        bool on_usb = b.valid && b.usb_power;
        if (on_usb && !was_on_usb) {
            synced_this_charge = false; /* just plugged in: sync once soon */
        }
        was_on_usb = on_usb;

        int64_t since_attempt_ms = attempted ? (esp_timer_get_time() - last_attempt_us) / 1000 : INT64_MAX;
        bool last_failed = s_state == WIFI_TIME_FAILED;
        bool due;
        if (on_usb) {
            due = !synced_this_charge
                      ? (!attempted || !last_failed || since_attempt_ms >= RETRY_INTERVAL_MS)
                      : since_attempt_ms >= RESYNC_INTERVAL_MS;
        } else {
            due = !s_time_known && since_attempt_ms >= RETRY_ON_BATTERY_MS;
        }

        if (due) {
            attempted = true;
            last_attempt_us = esp_timer_get_time();
            bool ok = sync_once();
            handle_sync_result(ok);
            if (ok && on_usb) {
                synced_this_charge = true;
            }
        } else if (s_state == WIFI_TIME_SYNCED && !on_usb) {
            s_state = WIFI_TIME_IDLE; /* not syncing on battery */
        }
        vTaskDelay(ms_to_ticks(CHECK_INTERVAL_MS));
    }
}

void wifi_time_start(bool time_known)
{
    s_time_known = time_known;

    esp_err_t err = nvs_flash_init(); /* Wi-Fi driver keeps calibration data in NVS */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, WIFI_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = strlen(WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_WATCH_NTP_SERVER);
    sntp.start = false; /* started per sync cycle, once we have an IP */
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));

    s_state = WIFI_TIME_IDLE;
    xTaskCreate(sync_task, "wifi_time", 4096, NULL, 3, NULL);
}

#else /* !HAVE_WIFI_SECRETS */

void wifi_time_start(bool time_known)
{
    (void)time_known;
    ESP_LOGW(TAG, "main/wifi_secrets.h not found: Wi-Fi time sync disabled. "
                  "Copy main/wifi_secrets.example.h to main/wifi_secrets.h and fill it in.");
    s_state = WIFI_TIME_NO_CONFIG;
}

#endif
