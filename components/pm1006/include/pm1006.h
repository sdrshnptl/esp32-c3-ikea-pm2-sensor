#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t pm25;       /*!< Latest PM2.5 reading in ug/m3 */
    bool valid;          /*!< At least one frame has been received */
    uint32_t age_ms;     /*!< Milliseconds since the last valid frame, UINT32_MAX if none */
    uint32_t frames_ok;  /*!< Valid frames received since boot */
    uint32_t frames_bad; /*!< Frames rejected by the checksum */
    uint32_t rx_bytes;   /*!< Raw bytes read from the UART */
} pm1006_reading_t;

/**
 * Start the UART receiver for the PM1006 sensor (IKEA VINDRIKTNING REST pad).
 *
 * The sensor streams 20-byte frames at 9600 8N1; this driver is receive-only.
 */
esp_err_t pm1006_init(void);

/**
 * Copy the most recent measurement. Returns ESP_ERR_INVALID_STATE before init.
 */
esp_err_t pm1006_get_reading(pm1006_reading_t *out);

#ifdef __cplusplus
}
#endif
