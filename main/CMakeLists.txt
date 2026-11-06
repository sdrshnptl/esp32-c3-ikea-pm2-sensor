idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        esp_app_format
        esp_event
        esp_hw_support
        esp_http_server
        esp_netif
        esp_wifi
        nvs_flash
    EMBED_TXTFILES "web/ota.html"
)
