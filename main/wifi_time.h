#pragma once

#include <stdbool.h>
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
 * Starts a background task that, on a schedule, turns Wi-Fi on, syncs the
 * clock over NTP, then turns Wi-Fi off again to save battery.
 * Requires nothing else to have initialised NVS / netif / the event loop.
 */
void wifi_time_start(void);

wifi_time_state_t wifi_time_state(void);

/* Unix time of the last successful sync, or 0 if never synced. */
time_t wifi_time_last_sync(void);
