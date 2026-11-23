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
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASSWORD_MAX_LEN 64
#define CONNECT_TIMEOUT_MS 15000
#define AP_OFF_DELAY_US (3 * 1000 * 1000)

/* Reconnection is event driven and backs off geometrically so a router that is
 * down does not turn into a connect storm: 5 s, 10 s, 20 s ... up to 5 min. */
#define RECONNECT_MIN_DELAY_US (5 * 1000 * 1000)
#define RECONNECT_MAX_DELAY_US (5 * 60 * 1000 * 1000)

/* After this many consecutive failures the provisioning AP is brought back so
 * the device remains reachable while the upstream network is unavailable. */
#define AP_RESCUE_AFTER_ATTEMPTS 4
#define AP_OFF_MAX_RETRIES 3

static const char *TAG = "wifi";

static SemaphoreHandle_t s_conn_sem;
static esp_timer_handle_t s_ap_off_timer;
static esp_timer_handle_t s_reconnect_timer;

static esp_ip4_addr_t s_ip;
static volatile bool s_sta_configured;
static volatile bool s_provisioning;
static bool s_rescue_ap;
static uint32_t s_failed_attempts;    /* consecutive failures, drives the backoff */
static uint32_t s_reconnect_attempts; /* retries since boot */
static uint32_t s_disconnects;        /* disconnect events since boot */
static uint8_t s_ap_off_retries;

/* True when the station holds an IP on the requested network. A queued event
 * from the previous link therefore cannot be mistaken for success. */
static bool link_matches(const char *ssid)
{
    wifi_ap_record_t ap;
    return s_ip.addr != 0 && esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
           strncmp((const char *)ap.ssid, ssid, sizeof(ap.ssid)) == 0;
}

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

static esp_err_t apply_sta_config(const char *ssid, const char *password)
{
    wifi_config_t sta_cfg = {0};
    snprintf((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), "%s", ssid);
    snprintf((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%s", password);
    return esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
}

static uint32_t next_reconnect_delay_us(void)
{
    uint32_t delay = RECONNECT_MIN_DELAY_US;
    for (uint32_t i = 0; i < s_failed_attempts && delay < RECONNECT_MAX_DELAY_US; i++) {
        delay *= 2;
    }
    return delay < RECONNECT_MAX_DELAY_US ? delay : RECONNECT_MAX_DELAY_US;
}

static void schedule_reconnect(void)
{
    if (!s_sta_configured) {
        return;
    }
    if (s_failed_attempts >= AP_RESCUE_AFTER_ATTEMPTS) {
        s_rescue_ap = true;
    }

    uint32_t delay_us = next_reconnect_delay_us();
    esp_timer_stop(s_reconnect_timer);
    if (esp_timer_start_once(s_reconnect_timer, delay_us) != ESP_OK) {
        ESP_LOGE(TAG, "Could not schedule the next reconnect");
        return;
    }
    ESP_LOGW(TAG, "Wi-Fi retry %lu in %lu s", (unsigned long)(s_failed_attempts + 1),
             (unsigned long)(delay_us / 1000000));
}

static void reconnect_timer_cb(void *arg)
{
    if (s_ip.addr != 0 || s_provisioning) {
        return; /* a late GOT_IP or a manual connect already took over */
    }

    if (s_rescue_ap) {
        s_rescue_ap = false;
        ESP_LOGW(TAG, "Wi-Fi unavailable, re-enabling provisioning AP");
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not enable provisioning AP: %s", esp_err_to_name(err));
        }
    }

    s_failed_attempts++;
    s_reconnect_attempts++;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        schedule_reconnect();
    }
}

static void ap_off_timer_cb(void *arg)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        s_ap_off_retries = 0;
        ESP_LOGI(TAG, "Switched to STA-only mode");
        return;
    }
    if (++s_ap_off_retries < AP_OFF_MAX_RETRIES) {
        ESP_LOGW(TAG, "Could not switch to STA-only mode: %s", esp_err_to_name(err));
        esp_timer_start_once(s_ap_off_timer, AP_OFF_DELAY_US);
        return;
    }
    s_ap_off_retries = 0;
    ESP_LOGE(TAG, "Keeping the provisioning AP on: %s", esp_err_to_name(err));
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *event = data;
    s_ip = event->ip_info.ip;
    s_failed_attempts = 0;
    s_rescue_ap = false;
    ESP_LOGI(TAG, "Connected to Wi-Fi, IP: " IPSTR, IP2STR(&s_ip));
    xSemaphoreGive(s_conn_sem);

    /* Drop a retry that was scheduled just before this event. */
    esp_timer_stop(s_reconnect_timer);
    esp_timer_stop(s_ap_off_timer);
    if (esp_timer_start_once(s_ap_off_timer, AP_OFF_DELAY_US) != ESP_OK) {
        ESP_LOGW(TAG, "Could not schedule AP shutdown");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *event = data;
        ESP_LOGI(TAG, "Associated with \"%.*s\"", event->ssid_len, (const char *)event->ssid);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *event = data;
        s_ip.addr = 0;
        s_disconnects++;
        esp_timer_stop(s_ap_off_timer);

        if (s_provisioning) {
            /* wifi_manager_connect() owns the retry decision while it waits. */
            ESP_LOGW(TAG, "Disconnected while joining \"%.*s\" (reason %u)", event->ssid_len,
                     (const char *)event->ssid, event->reason);
            break;
        }

        ESP_LOGW(TAG, "Disconnected from \"%.*s\" (reason %u, rssi %d)", event->ssid_len,
                 (const char *)event->ssid, event->reason, event->rssi);
        schedule_reconnect();
        break;
    }
    default:
        break;
    }
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
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "sta connected register");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                            wifi_event_handler, NULL, NULL),
                        TAG, "sta disconnected register");

    const esp_timer_create_args_t ap_off_args = {
        .callback = ap_off_timer_cb,
        .name = "ap_off",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&ap_off_args, &s_ap_off_timer), TAG, "ap timer create");

    const esp_timer_create_args_t reconnect_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&reconnect_args, &s_reconnect_timer), TAG, "retry timer create");

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

    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char password[WIFI_PASSWORD_MAX_LEN + 1] = {0};
    if (load_credentials(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK) {
        ESP_RETURN_ON_ERROR(apply_sta_config(ssid, password), TAG, "set STA config");
        s_sta_configured = true;
        ESP_LOGI(TAG, "Connecting to saved Wi-Fi: %s", ssid);
        if (esp_wifi_connect() != ESP_OK) {
            ESP_LOGW(TAG, "Could not start Wi-Fi connection, retrying in the background");
            schedule_reconnect();
        }
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi. Visit http://192.168.4.1/ to configure");
    }
    return ESP_OK;
}

esp_err_t wifi_manager_scan(wifi_ap_record_t *records, uint16_t max_count, uint16_t *out_count)
{
    if (records == NULL || out_count == NULL || max_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    wifi_scan_config_t scan_cfg = {0};
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        esp_wifi_clear_ap_list();
        return err;
    }

    uint16_t count = 0;
    if ((err = esp_wifi_scan_get_ap_num(&count)) != ESP_OK) {
        esp_wifi_clear_ap_list();
        return err;
    }
    if (count == 0) {
        esp_wifi_clear_ap_list(); /* nothing is fetched, so release the list here */
        return ESP_OK;
    }

    uint16_t fetch = count > max_count ? max_count : count;
    if ((err = esp_wifi_scan_get_ap_records(&fetch, records)) != ESP_OK) {
        esp_wifi_clear_ap_list();
        return err;
    }
    if (fetch < count) {
        esp_wifi_clear_ap_list(); /* release the records we did not take */
    }
    *out_count = fetch;
    return ESP_OK;
}

esp_err_t wifi_manager_get_ip(char *ip_out, size_t ip_len)
{
    if (ip_out == NULL || ip_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ip.addr == 0) {
        ip_out[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(ip_out, ip_len, IPSTR, IP2STR(&s_ip));
    return ESP_OK;
}

esp_err_t wifi_manager_get_status(wifi_manager_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (s_ip.addr != 0) {
        out->connected = true;
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&s_ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(out->ssid, sizeof(out->ssid), "%s", (const char *)ap.ssid);
            out->rssi = ap.rssi;
        }
    }
    out->disconnects = s_disconnects;
    out->reconnect_attempts = s_reconnect_attempts;
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password, char *ip_out, size_t ip_len)
{
    if (ip_out != NULL && ip_len > 0) {
        ip_out[0] = '\0';
    }
    if (ssid == NULL || ssid[0] == '\0' || password == NULL ||
        strlen(ssid) > WIFI_SSID_MAX_LEN || strlen(password) > WIFI_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    /* A provisioning attempt supersedes any pending background retry. */
    s_provisioning = true;
    esp_timer_stop(s_reconnect_timer);

    char prev_ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char prev_password[WIFI_PASSWORD_MAX_LEN + 1] = {0};
    bool have_previous = load_credentials(prev_ssid, sizeof(prev_ssid),
                                          prev_password, sizeof(prev_password)) == ESP_OK;

    esp_wifi_disconnect();

    esp_err_t err = apply_sta_config(ssid, password);
    if (err != ESP_OK) {
        s_provisioning = false;
        ESP_LOGE(TAG, "Set STA config failed: %s", esp_err_to_name(err));
        schedule_reconnect();
        return err;
    }
    s_sta_configured = true;

    xSemaphoreTake(s_conn_sem, 0);
    if ((err = esp_wifi_connect()) == ESP_OK) {
        int64_t deadline = esp_timer_get_time() + (int64_t)CONNECT_TIMEOUT_MS * 1000;
        while (!link_matches(ssid)) {
            int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
            if (left_ms <= 0 || xSemaphoreTake(s_conn_sem, pdMS_TO_TICKS(left_ms)) != pdTRUE) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
        }
    }
    s_provisioning = false;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not join \"%s\": %s", ssid, esp_err_to_name(err));
        if (!have_previous) {
            /* Nothing to fall back to; wait for new credentials. */
            s_sta_configured = false;
            apply_sta_config("", "");
            return err;
        }
        /* Roll back so the reconnect loop keeps the last known-good network. */
        if (apply_sta_config(prev_ssid, prev_password) != ESP_OK ||
            esp_wifi_connect() != ESP_OK) {
            schedule_reconnect();
        }
        return err;
    }

    if ((err = save_credentials(ssid, password)) != ESP_OK) {
        ESP_LOGE(TAG, "Could not save credentials: %s", esp_err_to_name(err));
    }
    if (ip_out != NULL && ip_len > 0) {
        snprintf(ip_out, ip_len, IPSTR, IP2STR(&s_ip));
    }
    return ESP_OK;
}
