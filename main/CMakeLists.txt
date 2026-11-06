idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        web
        wifi_manager
)
