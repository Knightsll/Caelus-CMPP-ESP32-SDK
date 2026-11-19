idf_component_register(
    SRCS
        "main.c"
        "board_config.c"
    INCLUDE_DIRS "."
    REQUIRES
        caelus_cmpp_sdk_esp32
    PRIV_REQUIRES
        caelus_cmpp_backend_pwm
        caelus_cmpp_backend_relay
        caelus_device_function
)
