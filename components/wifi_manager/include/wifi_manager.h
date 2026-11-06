#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_scan(wifi_ap_record_t *records, uint16_t max_count, uint16_t *out_count);
esp_err_t wifi_manager_connect(const char *ssid, const char *password, char *ip_out, size_t ip_len);

/* Copy the station IP address; ESP_ERR_INVALID_STATE while not connected. */
esp_err_t wifi_manager_get_ip(char *ip_out, size_t ip_len);

#ifdef __cplusplus
}
#endif
