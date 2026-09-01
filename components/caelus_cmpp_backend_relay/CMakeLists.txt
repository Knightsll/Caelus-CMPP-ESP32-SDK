idf_component_register(
    SRCS "src/cmpp_backend_relay.c" "src/cmpp_backend_relay_logic.c"
    INCLUDE_DIRS "include"
    REQUIRES caelus_cmpp_core esp_driver_gpio esp_timer
    PRIV_REQUIRES freertos
)
