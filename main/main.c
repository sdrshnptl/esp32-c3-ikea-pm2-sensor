#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "t01c3_ota";

extern const char ota_html_start[] asm("_binary_ota_html_start");
extern const char ota_html_end[] asm("_binary_ota_html_end");

static uint8_t s_ota_buffer[4096];

static esp_err_t send_upload_error(httpd_req_t *request,
                                   httpd_err_code_t status,
                                   const char *message)
{
    esp_err_t err = httpd_resp_send_err(request, status, message);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not send HTTP error response: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t ota_page_handler(httpd_req_t *request)
{
    esp_err_t err = httpd_resp_set_type(request, "text/html; charset=utf-8");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not set OTA page content type: %s", esp_err_to_name(err));
        return err;
    }
    return httpd_resp_send(request, ota_html_start, ota_html_end - ota_html_start);
}

static esp_err_t ota_upload_handler(httpd_req_t *request)
{
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA update partition is available");
        return send_upload_error(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                 "No OTA partition available");
    }

    if (request->content_len == 0 || request->content_len > update_partition->size) {
        ESP_LOGW(TAG, "Rejected firmware upload size: %u bytes",
                 (unsigned)request->content_len);
        return send_upload_error(request, HTTPD_400_BAD_REQUEST,
                                 "Invalid firmware size");
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, request->content_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not begin OTA: %s", esp_err_to_name(err));
        return send_upload_error(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                 "Could not start OTA");
    }

    size_t remaining = request->content_len;
    while (remaining > 0) {
        size_t chunk_size = remaining < sizeof(s_ota_buffer)
                                ? remaining
                                : sizeof(s_ota_buffer);
        int received = httpd_req_recv(request, (char *)s_ota_buffer, chunk_size);
        if (received <= 0) {
            ESP_LOGE(TAG, "Firmware upload receive failed: %d", received);
            esp_err_t abort_err = esp_ota_abort(ota_handle);
            if (abort_err != ESP_OK) {
                ESP_LOGE(TAG, "Could not abort OTA: %s", esp_err_to_name(abort_err));
            }
            return send_upload_error(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                     "Firmware upload interrupted");
        }

        err = esp_ota_write(ota_handle, s_ota_buffer, (size_t)received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not write firmware: %s", esp_err_to_name(err));
            esp_err_t abort_err = esp_ota_abort(ota_handle);
            if (abort_err != ESP_OK) {
                ESP_LOGE(TAG, "Could not abort OTA: %s", esp_err_to_name(abort_err));
            }
            return send_upload_error(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                     "Could not write firmware");
        }
        remaining -= (size_t)received;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Firmware image validation failed: %s", esp_err_to_name(err));
        return send_upload_error(request, HTTPD_400_BAD_REQUEST,
                                 "Invalid firmware image");
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not select new firmware: %s", esp_err_to_name(err));
        return send_upload_error(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                 "Could not select new firmware");
    }

    ESP_LOGI(TAG, "Firmware accepted; rebooting into %s", update_partition->label);
    err = httpd_resp_sendstr(request, "Firmware accepted. The device is restarting.");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not send OTA success response: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t load_or_create_ap_password(char password[17])
{
    static const char password_alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("ota", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t password_length = 17;
    err = nvs_get_str(nvs_handle, "ap_password", password, &password_length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t random_bytes[16];
        esp_fill_random(random_bytes, sizeof(random_bytes));
        for (size_t i = 0; i < sizeof(random_bytes); ++i) {
            password[i] = password_alphabet[random_bytes[i] & 0x3f];
        }
        password[sizeof(random_bytes)] = '\0';

        err = nvs_set_str(nvs_handle, "ap_password", password);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
        }
    } else if (err == ESP_OK &&
               (password_length != 17 || strlen(password) != 16)) {
        err = ESP_ERR_INVALID_SIZE;
    }

    nvs_close(nvs_handle);
    return err;
}

static httpd_handle_t start_ota_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t page_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ota_page_handler,
    };
    const httpd_uri_t upload_uri = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &page_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &upload_uri));
    return server;
}

static void start_wifi_access_point(void)
{
    char password[17] = {0};
    uint8_t mac[6];

    ESP_ERROR_CHECK(load_or_create_ap_password(password));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (netif == NULL) {
        ESP_LOGE(TAG, "Could not create Wi-Fi access point network interface");
        abort();
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));

    char ssid[20];
    int ssid_length = snprintf(ssid, sizeof(ssid), "T01C3-%02X%02X%02X",
                               mac[3], mac[4], mac[5]);
    if (ssid_length < 0 || (size_t)ssid_length >= sizeof(ssid)) {
        ESP_LOGE(TAG, "Could not format access point name");
        abort();
    }

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = (uint8_t)ssid_length,
            .channel = 1,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    memcpy(wifi_config.ap.ssid, ssid, (size_t)ssid_length);
    memcpy(wifi_config.ap.password, password, sizeof(password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi network: %s", ssid);
    ESP_LOGI(TAG, "Wi-Fi password (save for future connections): %s", password);
    ESP_LOGI(TAG, "Connect to http://192.168.4.1/ to upload firmware");
}

static void confirm_running_firmware(void)
{
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    if (running_partition == NULL) {
        ESP_LOGE(TAG, "Could not identify the running firmware partition");
        return;
    }

    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running_partition, &state);
    if (err == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not confirm running firmware: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Running firmware confirmed");
        }
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not query OTA state: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    const esp_app_desc_t *app_description = esp_app_get_description();
    ESP_LOGI(TAG, "Starting %s version %s", app_description->project_name,
             app_description->version);

    start_wifi_access_point();
    (void)start_ota_server();
    confirm_running_firmware();
}
