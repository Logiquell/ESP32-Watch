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
#include "rtc.h"

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

static EventGroupHandle_t s_events;
static volatile wifi_time_state_t s_state = WIFI_TIME_NO_CONFIG;
static volatile time_t s_last_sync;
static volatile bool s_want_connection;

wifi_time_state_t wifi_time_state(void) { return s_state; }
time_t wifi_time_last_sync(void) { return s_last_sync; }

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
    return ok;
}

static void sync_task(void *arg)
{
    for (;;) {
        if (sync_once()) {
            s_last_sync = time(NULL);
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
            vTaskDelay(pdMS_TO_TICKS(RESYNC_INTERVAL_MS));
        } else {
            s_state = WIFI_TIME_FAILED;
            vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
        }
    }
}

void wifi_time_start(void)
{
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

void wifi_time_start(void)
{
    ESP_LOGW(TAG, "main/wifi_secrets.h not found: Wi-Fi time sync disabled. "
                  "Copy main/wifi_secrets.example.h to main/wifi_secrets.h and fill it in.");
    s_state = WIFI_TIME_NO_CONFIG;
}

#endif
