idf_component_register(
    SRCS "src/cmpp_adapter_mcp.c" "src/cmpp_mcp_binding.c"
    INCLUDE_DIRS "include"
    REQUIRES caelus_cmpp_core caelus_cmpp_codec_json mcp-c-sdk cjson
    PRIV_REQUIRES freertos
)
