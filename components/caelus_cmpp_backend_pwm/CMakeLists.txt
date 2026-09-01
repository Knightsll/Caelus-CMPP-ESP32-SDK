idf_component_register(
    SRCS "src/cmpp_backend_pwm.c" "src/cmpp_backend_pwm_logic.c"
    INCLUDE_DIRS "include"
    REQUIRES caelus_cmpp_core esp_driver_ledc
    PRIV_REQUIRES freertos
)
