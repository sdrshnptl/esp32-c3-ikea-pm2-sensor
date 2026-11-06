#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "web.h"
#include "wifi_manager.h"

static const char *TAG = "app";

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
    ESP_ERROR_CHECK(web_start());
    confirm_ota();
}
