idf_component_register(
    SRCS "src/pm1006.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_timer)
