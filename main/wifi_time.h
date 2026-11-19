#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    WIFI_TIME_NO_CONFIG,  /* main/wifi_secrets.h missing: Wi-Fi disabled */
    WIFI_TIME_IDLE,       /* radio off between syncs */
    WIFI_TIME_CONNECTING, /* joining the access point */
    WIFI_TIME_SYNCING,    /* connected, waiting for NTP reply */
    WIFI_TIME_SYNCED,     /* last attempt succeeded (radio now off) */
    WIFI_TIME_FAILED,     /* last attempt failed; will retry later */
} wifi_time_state_t;

/*
 * Starts a background task that syncs the clock over NTP, turning Wi-Fi on only for the
 * sync. It syncs only while USB is connected (soon after plugging in, then every
 * CONFIG_WATCH_RESYNC_HOURS), or on battery if time_known is false (RTC had no valid time).
 * Requires nothing else to have initialised NVS / netif / the event loop.
 */
void wifi_time_start(bool time_known);

wifi_time_state_t wifi_time_state(void);

/* Unix time of the last successful sync, or 0 if never synced. */
time_t wifi_time_last_sync(void);

typedef struct {
    uint32_t attempts;
    uint32_t failures;
    uint32_t radio_on_ms;            /* total time the Wi-Fi radio was on */
    uint8_t  last_disconnect_reason; /* Wi-Fi driver reason code, 0 = none */
} wifi_time_stats_t;

wifi_time_stats_t wifi_time_stats(void);
