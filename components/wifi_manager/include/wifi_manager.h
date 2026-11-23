#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool connected;              /*!< Station currently holds an IP lease */
    char ssid[33];               /*!< SSID of the associated AP, empty while down */
    char ip[16];                 /*!< Current IPv4 address, empty while down */
    int8_t rssi;                 /*!< AP RSSI in dBm, 0 while down */
    uint32_t disconnects;        /*!< Disconnect events seen since boot */
    uint32_t reconnect_attempts; /*!< Reconnect attempts since boot */
} wifi_manager_status_t;

esp_err_t wifi_manager_init(void);

/**
 * Scan for nearby APs.
 *
 * The scan result list held by the driver is always released, also when no AP
 * is found or the scan fails.
 */
esp_err_t wifi_manager_scan(wifi_ap_record_t *records, uint16_t max_count, uint16_t *out_count);

/**
 * Join a network and wait up to 15 s for an IP address.
 *
 * Credentials are persisted only after the link succeeds; otherwise the
 * previously stored network is restored. Link loss is handled in the
 * background with exponential backoff (5 s up to 5 min), and after a few
 * failed attempts the provisioning AP is re-enabled so the device stays
 * reachable on 192.168.4.1.
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password, char *ip_out, size_t ip_len);

/* Copy the station IP address; ESP_ERR_INVALID_STATE while not connected. */
esp_err_t wifi_manager_get_ip(char *ip_out, size_t ip_len);

/* Snapshot of the link state plus reconnect counters. */
esp_err_t wifi_manager_get_status(wifi_manager_status_t *out);

#ifdef __cplusplus
}
#endif
