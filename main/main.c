#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"

#include "pm1006.h"
#include "web.h"
#include "wifi_manager.h"

static const char *TAG = "app";

#define MDNS_HOSTNAME "aqi"
#define MDNS_INSTANCE "T-01C3 Air Quality Monitor"

#define STATUS_LOG_INTERVAL_MS (60 * 1000)
#define STATUS_TASK_STACK_SIZE 4096

static void mdns_hostname_changed(const char *hostname, void *arg)
{
    ESP_LOGW(TAG, "mDNS hostname changed to \"%s.local\" (name taken on this network)", hostname);
}

static esp_err_t start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }
    if ((err = mdns_hostname_set(MDNS_HOSTNAME)) == ESP_OK) {
        err = mdns_instance_name_set(MDNS_INSTANCE);
    }
    if (err == ESP_OK && !mdns_service_exists("_http", "_tcp", NULL)) {
        err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS announcement failed: %s", esp_err_to_name(err));
        return err;
    }

    mdns_register_hostname_changed_callback(mdns_hostname_changed, NULL);

    char hostname[MDNS_NAME_BUF_LEN] = {0};
    if (mdns_hostname_get(hostname) == ESP_OK) {
        ESP_LOGI(TAG, "mDNS ready: http://%s.local", hostname);
    }
    return ESP_OK;
}

static void log_status(void)
{
    wifi_manager_status_t wifi;
    wifi_manager_get_status(&wifi);

    char hostname[MDNS_NAME_BUF_LEN] = {0};
    bool mdns_ok = mdns_hostname_get(hostname) == ESP_OK;
    if (!mdns_ok) {
        ESP_LOGW(TAG, "mDNS is not running, starting it again");
        mdns_ok = start_mdns() == ESP_OK && mdns_hostname_get(hostname) == ESP_OK;
    } else if (!mdns_service_exists("_http", "_tcp", NULL)) {
        ESP_LOGW(TAG, "mDNS service missing, re-adding");
        mdns_ok = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0) == ESP_OK;
    }

    ESP_LOGI(TAG, "wifi=%s ip=%s rssi=%d reconnects=%lu heap=%lu min_heap=%lu mdns=%s%s",
             wifi.connected ? "up" : "down", wifi.connected ? wifi.ip : "-", wifi.rssi,
             (unsigned long)wifi.reconnect_attempts, (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(), mdns_ok ? hostname : "unavailable",
             mdns_ok ? ".local" : "");
}

static void status_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_LOG_INTERVAL_MS));
        log_status();
    }
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

    if (xTaskCreate(status_task, "status", STATUS_TASK_STACK_SIZE, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Status logging disabled: no memory for the task");
    }

    confirm_ota();
}
