idf_component_register(
    SRCS "src/test_board_device.c"
    INCLUDE_DIRS "include"
    REQUIRES caelus_cmpp_core caelus_cmpp_backend_pwm caelus_cmpp_backend_relay
)

