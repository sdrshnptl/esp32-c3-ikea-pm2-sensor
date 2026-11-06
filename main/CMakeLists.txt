idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        pm1006
        web
        wifi_manager
)
