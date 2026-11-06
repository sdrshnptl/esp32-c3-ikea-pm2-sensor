#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "mdns.h"

#include "pm1006.h"
#include "web.h"
#include "wifi_manager.h"

static const char *TAG = "app";

#define MDNS_HOSTNAME "aqi"
#define MDNS_INSTANCE "T-01C3 Air Quality Monitor"

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    if ((err = mdns_hostname_set(MDNS_HOSTNAME)) != ESP_OK ||
        (err = mdns_instance_name_set(MDNS_INSTANCE)) != ESP_OK ||
        (err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0)) != ESP_OK) {
        ESP_LOGE(TAG, "mDNS announcement failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "mDNS ready: http://%s.local", MDNS_HOSTNAME);
}

static void confirm_ota(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != NULL &&
        esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        ESP_LOGI(TAG, "Firmware confirmed");
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(wifi_manager_init());
    start_mdns();

    esp_err_t sensor_err = pm1006_init();
    if (sensor_err != ESP_OK) {
        ESP_LOGE(TAG, "PM1006 init failed: %s", esp_err_to_name(sensor_err));
    }

    ESP_ERROR_CHECK(web_start());
    confirm_ota();
}
