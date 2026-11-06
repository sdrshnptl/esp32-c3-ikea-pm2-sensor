#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_manager.h"

#define WIFI_NVS_NAMESPACE "wifi"
#define CONNECT_TIMEOUT_MS 15000
#define AP_OFF_DELAY_US (3 * 1000 * 1000)

static const char *TAG = "wifi";

static SemaphoreHandle_t s_conn_sem;
static esp_timer_handle_t s_ap_off_timer;
static char s_ip[16];

static esp_err_t save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "nvs open");
    esp_err_t err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "password", password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t load_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle), TAG, "nvs open");
    esp_err_t err = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "password", password, &password_len);
    }
    nvs_close(handle);
    return err;
}

static void ap_off_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "Switching to STA-only mode");
    esp_wifi_set_mode(WIFI_MODE_STA);
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *event = data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "Connected to Wi-Fi, IP: %s", s_ip);
    xSemaphoreGive(s_conn_sem);
    esp_timer_start_once(s_ap_off_timer, AP_OFF_DELAY_US);
}

esp_err_t wifi_manager_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    s_conn_sem = xSemaphoreCreateBinary();
    if (s_conn_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            ip_event_handler, NULL, NULL),
                        TAG, "ip event register");

    const esp_timer_create_args_t timer_args = {
        .callback = ap_off_timer_cb,
        .name = "ap_off",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_ap_off_timer), TAG, "timer create");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set mode");

    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_AP, mac), TAG, "get mac");

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = 1,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    int ssid_len = snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid),
                            "T01C3-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ap_cfg.ap.ssid_len = (uint8_t)ssid_len;
    snprintf((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), "%s", "password");

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    ESP_LOGI(TAG, "AP: %s / password: password", ap_cfg.ap.ssid);

    char ssid[32] = {0};
    char password[64] = {0};
    if (load_credentials(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK) {
        wifi_config_t sta_cfg = {0};
        snprintf((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), "%s", ssid);
        snprintf((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%s", password);
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "set STA config");
        if (esp_wifi_connect() != ESP_OK) {
            ESP_LOGW(TAG, "Could not start Wi-Fi connection");
        }
        ESP_LOGI(TAG, "Connecting to saved Wi-Fi: %s", ssid);
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi. Visit http://192.168.4.1/ to configure");
    }
    return ESP_OK;
}

esp_err_t wifi_manager_scan(wifi_ap_record_t *records, uint16_t max_count, uint16_t *out_count)
{
    *out_count = 0;
    wifi_scan_config_t scan_cfg = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_cfg, true), TAG, "scan start");

    uint16_t count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&count), TAG, "get ap num");
    if (count > max_count) {
        count = max_count;
    }
    if (count > 0) {
        ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&count, records), TAG, "get ap records");
    }
    *out_count = count;
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password, char *ip_out, size_t ip_len)
{
    if (ip_out != NULL && ip_len > 0) {
        ip_out[0] = '\0';
    }

    ESP_RETURN_ON_ERROR(save_credentials(ssid, password), TAG, "save credentials");

    wifi_config_t sta_cfg = {0};
    snprintf((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), "%s", ssid);
    snprintf((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%s", password);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "set STA config");

    xSemaphoreTake(s_conn_sem, 0);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_conn_sem, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (ip_out != NULL && ip_len > 0) {
        snprintf(ip_out, ip_len, "%s", s_ip);
    }
    return ESP_OK;
}
