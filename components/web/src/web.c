#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pm1006.h"
#include "wifi_manager.h"

static const char *TAG = "web";

extern const char dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const char dashboard_html_end[] asm("_binary_dashboard_html_end");
extern const char settings_html_start[] asm("_binary_settings_html_start");
extern const char settings_html_end[] asm("_binary_settings_html_end");

/* The VINDRIKTNING control board emits frames every few seconds; a reading
 * older than this is treated as a dropped sensor link. */
#define SENSOR_ONLINE_TIMEOUT_MS 30000

static uint8_t s_ota_buffer[4096];

static esp_err_t send_html(httpd_req_t *req, const char *start, const char *end)
{
    esp_err_t err = httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send(req, start, end - start - 1);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    return send_html(req, dashboard_html_start, dashboard_html_end);
}

static esp_err_t settings_handler(httpd_req_t *req)
{
    return send_html(req, settings_html_start, settings_html_end);
}

static void json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t out = 0;
    for (; *src != '\0' && out + 2 < dst_len; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[out++] = '\\';
            dst[out++] = (char)c;
        } else {
            dst[out++] = c < 0x20 ? '?' : (char)c;
        }
    }
    dst[out] = '\0';
}

static esp_err_t data_handler(httpd_req_t *req)
{
    pm1006_reading_t reading = {0};
    if (pm1006_get_reading(&reading) != ESP_OK) {
        reading.age_ms = UINT32_MAX;
    }
    bool online = reading.valid && reading.age_ms <= SENSOR_ONLINE_TIMEOUT_MS;

    char ssid[36] = {0};
    int rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        json_escape(ssid, sizeof(ssid), (const char *)ap.ssid);
        rssi = ap.rssi;
    }

    char ip[16] = {0};
    wifi_manager_get_ip(ip, sizeof(ip));

    const esp_app_desc_t *app = esp_app_get_description();
    char json[512];
    int len = snprintf(json, sizeof(json),
                       "{\"pm25\":%u,\"sensor_online\":%s,\"age_ms\":%lu,"
                       "\"frames_ok\":%lu,\"frames_bad\":%lu,\"rx_bytes\":%lu,\"uptime_s\":%lu,"
                       "\"heap\":%lu,\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\","
                       "\"fw\":\"%s\",\"idf\":\"%s\"}",
                       reading.pm25, online ? "true" : "false", (unsigned long)reading.age_ms,
                       (unsigned long)reading.frames_ok, (unsigned long)reading.frames_bad,
                       (unsigned long)reading.rx_bytes,
                       (unsigned long)(esp_timer_get_time() / 1000000),
                       (unsigned long)esp_get_free_heap_size(), ssid, rssi, ip, app->version,
                       app->idf_ver);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, len);
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    wifi_ap_record_t records[16];
    uint16_t count = 0;
    esp_err_t err = wifi_manager_scan(records, 16, &count);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    }

    char json[2048];
    int off = snprintf(json, sizeof(json), "[");
    for (uint16_t i = 0; i < count; i++) {
        off += snprintf(json + off, sizeof(json) - off, "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                        i ? "," : "", records[i].ssid, records[i].rssi);
    }
    off += snprintf(json + off, sizeof(json) - off, "]");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, off);
}

static esp_err_t connect_handler(httpd_req_t *req)
{
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
    }

    char ssid[33] = {0};
    char password[65] = {0};
    if (httpd_query_key_value(query, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid missing");
    }
    httpd_query_key_value(query, "password", password, sizeof(password));

    char ip[16] = {0};
    esp_err_t err = wifi_manager_connect(ssid, password, ip, sizeof(ip));
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "connection failed");
    }

    char body[96];
    int len = snprintf(body, sizeof(body), "Connected. New IP: %s", ip);
    return httpd_resp_send(req, body, len);
}

static esp_err_t update_handler(httpd_req_t *req)
{
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
    }
    if (req->content_len == 0 || req->content_len > update_partition->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid size");
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
    }

    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(s_ota_buffer) ? remaining : sizeof(s_ota_buffer);
        int received = httpd_req_recv(req, (char *)s_ota_buffer, chunk);
        if (received <= 0) {
            esp_ota_abort(ota_handle);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        }
        err = esp_ota_write(ota_handle, s_ota_buffer, (size_t)received);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        }
        remaining -= (size_t)received;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid image");
    }
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot failed");
    }

    ESP_LOGI(TAG, "Firmware accepted, restarting");
    httpd_resp_sendstr(req, "Firmware accepted. Restarting.");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

esp_err_t web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd start");

    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    const httpd_uri_t settings = { .uri = "/settings", .method = HTTP_GET, .handler = settings_handler };
    const httpd_uri_t data = { .uri = "/api/data", .method = HTTP_GET, .handler = data_handler };
    const httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET, .handler = scan_handler };
    const httpd_uri_t conn = { .uri = "/connect", .method = HTTP_POST, .handler = connect_handler };
    const httpd_uri_t update = { .uri = "/update", .method = HTTP_POST, .handler = update_handler };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root), TAG, "register /");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &settings), TAG, "register /settings");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &data), TAG, "register /api/data");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &scan), TAG, "register /scan");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &conn), TAG, "register /connect");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &update), TAG, "register /update");
    return ESP_OK;
}
