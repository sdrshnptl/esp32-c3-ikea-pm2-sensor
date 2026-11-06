#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pm1006.h"

#define PM1006_UART_PORT CONFIG_PM1006_UART_PORT
#define PM1006_RX_GPIO CONFIG_PM1006_UART_RX_GPIO

#define PM1006_FRAME_LEN 20
#define PM1006_HEADER_LEN 3
#define PM1006_RX_BUFFER_SIZE 512
#define PM1006_TASK_STACK_SIZE 3072
#define PM1006_TASK_PRIORITY 5
#define PM1006_READ_TIMEOUT_MS 1000
#define PM1006_LOG_INTERVAL_US (60 * 1000 * 1000)

static const char *TAG = "pm1006";

static const uint8_t s_header[PM1006_HEADER_LEN] = {0x16, 0x11, 0x0B};

typedef struct {
    uint8_t frame[PM1006_FRAME_LEN];
    size_t len;
} pm1006_parser_t;

static pm1006_parser_t s_parser;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static struct {
    uint16_t pm25;
    uint64_t last_ok_us;
    uint32_t frames_ok;
    uint32_t frames_bad;
    uint32_t rx_bytes;
} s_state;

static bool s_started;

typedef enum {
    PM1006_FRAME_NONE,
    PM1006_FRAME_OK,
    PM1006_FRAME_BAD,
} pm1006_frame_result_t;

static pm1006_frame_result_t parser_feed(pm1006_parser_t *parser, uint8_t byte, uint16_t *pm25)
{
    if (parser->len < PM1006_HEADER_LEN && byte != s_header[parser->len]) {
        parser->len = 0;
        if (byte == s_header[0]) {
            parser->frame[parser->len++] = byte;
        }
        return PM1006_FRAME_NONE;
    }

    parser->frame[parser->len++] = byte;
    if (parser->len < PM1006_FRAME_LEN) {
        return PM1006_FRAME_NONE;
    }
    parser->len = 0;

    uint8_t sum = 0;
    for (size_t i = 0; i < PM1006_FRAME_LEN; i++) {
        sum += parser->frame[i];
    }
    if (sum != 0) {
        return PM1006_FRAME_BAD;
    }

    /* Bytes 5-6 hold PM2.5 in ug/m3, big endian. */
    *pm25 = ((uint16_t)parser->frame[5] << 8) | parser->frame[6];
    return PM1006_FRAME_OK;
}

static void pm1006_task(void *arg)
{
    static uint8_t rx[64];
    uint64_t last_log_us = esp_timer_get_time();
    bool logged_first_frame = false;

    for (;;) {
        int received = uart_read_bytes(PM1006_UART_PORT, rx, sizeof(rx), pdMS_TO_TICKS(PM1006_READ_TIMEOUT_MS));
        uint16_t pm25 = 0;

        if (received > 0) {
            portENTER_CRITICAL(&s_lock);
            s_state.rx_bytes += (uint32_t)received;
            portEXIT_CRITICAL(&s_lock);
        }

        for (int i = 0; i < received; i++) {
            pm1006_frame_result_t result = parser_feed(&s_parser, rx[i], &pm25);
            if (result == PM1006_FRAME_OK) {
                portENTER_CRITICAL(&s_lock);
                s_state.pm25 = pm25;
                s_state.last_ok_us = esp_timer_get_time();
                s_state.frames_ok++;
                portEXIT_CRITICAL(&s_lock);

                if (!logged_first_frame) {
                    logged_first_frame = true;
                    ESP_LOGI(TAG, "Sensor online on GPIO%d", PM1006_RX_GPIO);
                }
            } else if (result == PM1006_FRAME_BAD) {
                portENTER_CRITICAL(&s_lock);
                s_state.frames_bad++;
                portEXIT_CRITICAL(&s_lock);
            }
        }

        uint64_t now_us = esp_timer_get_time();
        if (now_us - last_log_us >= PM1006_LOG_INTERVAL_US) {
            last_log_us = now_us;
            pm1006_reading_t reading;
            if (pm1006_get_reading(&reading) == ESP_OK) {
                if (reading.valid) {
                    ESP_LOGI(TAG, "PM2.5 %u ug/m3 (frames ok=%lu bad=%lu)", reading.pm25,
                             (unsigned long)reading.frames_ok, (unsigned long)reading.frames_bad);
                } else {
                    ESP_LOGW(TAG, "No frames from sensor on GPIO%d", PM1006_RX_GPIO);
                }
            }
        }
    }
}

esp_err_t pm1006_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    const uart_config_t config = {
        .baud_rate = CONFIG_PM1006_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(PM1006_UART_PORT, PM1006_RX_BUFFER_SIZE, 0, 0, NULL, 0),
                        TAG, "uart driver install");
    ESP_RETURN_ON_ERROR(uart_param_config(PM1006_UART_PORT, &config), TAG, "uart param config");
    ESP_RETURN_ON_ERROR(uart_set_pin(PM1006_UART_PORT, UART_PIN_NO_CHANGE, PM1006_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart pin config");
    uart_flush_input(PM1006_UART_PORT);

    memset(&s_parser, 0, sizeof(s_parser));
    if (xTaskCreate(pm1006_task, "pm1006", PM1006_TASK_STACK_SIZE, NULL, PM1006_TASK_PRIORITY, NULL) != pdPASS) {
        uart_driver_delete(PM1006_UART_PORT);
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "Listening for PM1006 frames on GPIO%d at %d baud", PM1006_RX_GPIO, CONFIG_PM1006_UART_BAUD);
    return ESP_OK;
}

esp_err_t pm1006_get_reading(pm1006_reading_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t pm25;
    uint64_t last_ok_us;
    uint32_t frames_ok;
    uint32_t frames_bad;
    uint32_t rx_bytes;

    portENTER_CRITICAL(&s_lock);
    pm25 = s_state.pm25;
    last_ok_us = s_state.last_ok_us;
    frames_ok = s_state.frames_ok;
    frames_bad = s_state.frames_bad;
    rx_bytes = s_state.rx_bytes;
    portEXIT_CRITICAL(&s_lock);

    out->pm25 = pm25;
    out->valid = last_ok_us != 0;
    out->frames_ok = frames_ok;
    out->frames_bad = frames_bad;
    out->rx_bytes = rx_bytes;
    out->age_ms = out->valid ? (uint32_t)((esp_timer_get_time() - (int64_t)last_ok_us) / 1000) : UINT32_MAX;
    return ESP_OK;
}
