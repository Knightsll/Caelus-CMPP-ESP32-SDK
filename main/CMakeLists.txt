set(cmpp_board_sources
        "main.c"
        "board_config.c"
        "cmpp_device_server.c"
        "device_identity.c"
        "cmpp_mqtt_client.c"
        "network.c")

set(cmpp_board_requires
        cjson
        dns_server
        esp_app_format
        esp_driver_gpio
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        lwip
        mbedtls
        mqtt
        nvs_flash
        mdns
        mcp-c-sdk
        network_provisioning)

set(cmpp_board_private_requires
        caelus_cmpp_adapter_mcp
        caelus_cmpp_adapter_mqtt
        caelus_cmpp_backend_pwm
        caelus_cmpp_backend_relay
        caelus_cmpp_codec_json
        caelus_cmpp_platform_esp32
        caelus_device_function)

if(CONFIG_CMPP_BOARD_MQTT_ENABLE)
    list(APPEND cmpp_board_sources "mqtt_settings.c")
endif()

idf_component_register(
    SRCS ${cmpp_board_sources}
    INCLUDE_DIRS "."
    EMBED_FILES
        "portal.html"
    REQUIRES ${cmpp_board_requires}
    PRIV_REQUIRES ${cmpp_board_private_requires}
)
