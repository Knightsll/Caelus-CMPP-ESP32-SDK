idf_component_register(
    SRCS "src/cmpp_platform_esp32.c"
    INCLUDE_DIRS "include"
    REQUIRES caelus_cmpp_core esp_timer nvs_flash
    PRIV_REQUIRES esp_system freertos log
)

