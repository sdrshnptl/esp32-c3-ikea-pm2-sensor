idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        mdns
        pm1006
        web
        wifi_manager
)
