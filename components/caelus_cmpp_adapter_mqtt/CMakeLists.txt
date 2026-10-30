idf_component_register(
    SRCS "src/cmpp_mqtt_binding.c"
    INCLUDE_DIRS "include"
    REQUIRES caelus_cmpp_core caelus_cmpp_codec_json
)
