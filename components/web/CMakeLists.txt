idf_component_register(
    SRCS "src/web.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_app_format esp_http_server esp_system esp_timer esp_wifi pm1006 wifi_manager
    EMBED_TXTFILES "web/dashboard.html" "web/settings.html")
