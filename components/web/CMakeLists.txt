idf_component_register(
    SRCS "src/web.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_http_server esp_system esp_wifi wifi_manager
    EMBED_TXTFILES "web/provision.html")
