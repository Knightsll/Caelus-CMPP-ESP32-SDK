idf_component_register(
    SRCS
        "src/cmpp_device_server.c"
        "src/cmpp_mqtt_client.c"
        "src/cmpp_sdk_esp32.c"
        "src/device_identity.c"
        "src/mqtt_settings.c"
        "src/network.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    EMBED_FILES "portal.html"
    REQUIRES
        caelus_cmpp_core
        esp_common
    PRIV_REQUIRES
        caelus_cmpp_adapter_mcp
        caelus_cmpp_adapter_mqtt
        caelus_cmpp_codec_json
        caelus_cmpp_platform_esp32
        cjson
        dns_server
        esp_app_format
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
        network_provisioning
)
