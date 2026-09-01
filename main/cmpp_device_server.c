/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp_device_server.h"

#include <stdio.h>
#include <string.h>

#include "cmpp/cmpp_adapter_mcp.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_mgr.h"
#include "mdns.h"

static const char *TAG = "cmpp_device_server";
static device_identity_t s_identity;
static esp_mcp_t *s_mcp;
static cmpp_mcp_adapter_t *s_adapter;
static esp_mcp_mgr_handle_t s_manager;

static bool constant_time_token_equal(const char *provided,
                                      const char *expected)
{
    if (!provided || !expected) {
        return false;
    }
    const size_t provided_length = strlen(provided);
    const size_t expected_length = strlen(expected);
    volatile unsigned char difference =
        (unsigned char)(provided_length ^ expected_length);
    for (size_t index = 0; index < expected_length; ++index) {
        const unsigned char provided_byte =
            index < provided_length ? (unsigned char)provided[index] : 0U;
        difference |= provided_byte ^ (unsigned char)expected[index];
    }
    return difference == 0U;
}

static bool validate_bearer_token(const char *bearer_token,
                                  char *out_subject,
                                  size_t out_subject_length,
                                  void *context)
{
    (void)context;
    if (!constant_time_token_equal(bearer_token, s_identity.auth_token)) {
        return false;
    }
    if (out_subject && out_subject_length > 0U) {
        (void)snprintf(out_subject, out_subject_length, "%s",
                       s_identity.device_uid);
    }
    return true;
}

static esp_err_t start_mdns(void)
{
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mDNS init failed");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(s_identity.hostname), TAG,
                        "mDNS hostname failed");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(s_identity.display_name), TAG,
                        "mDNS instance failed");
    bool paired = false;
    (void)device_identity_pairing_complete(&paired);
    char port_text[8];
    (void)snprintf(port_text, sizeof(port_text), "%d",
                   CONFIG_CMPP_BOARD_HTTP_PORT);
    mdns_txt_item_t records[] = {
        {"path", "/mcp"},
        {"transport", "streamable-http"},
        {"device_type", "cmpp-test-board"},
        {"cmpp_version", CMPP_PROTOCOL_VERSION},
        {"mcp_version", cmpp_mcp_protocol_version()},
        {"device_uid", s_identity.device_uid},
        {"display_name", s_identity.display_name},
        {"pairing", paired ? "paired" : "unpaired"},
        {"auth", "local_token"},
        {"port", port_text},
        {"firmware", esp_app_get_description()->version},
    };
    return mdns_service_add(s_identity.display_name, "_cmpp", "_tcp",
                            CONFIG_CMPP_BOARD_HTTP_PORT, records,
                            sizeof(records) / sizeof(records[0]));
}

static esp_err_t esp_error_from_cmpp(cmpp_status_t status)
{
    return status == CMPP_OK
               ? ESP_OK
               : status == CMPP_ERR_NO_MEMORY ? ESP_ERR_NO_MEM
                                               : ESP_FAIL;
}

esp_err_t cmpp_device_server_start(const device_identity_t *identity,
                                   cmpp_runtime_t *runtime)
{
    ESP_RETURN_ON_FALSE(identity && runtime, ESP_ERR_INVALID_ARG, TAG,
                        "Identity and runtime are required");
#if !CONFIG_MCP_HTTP_AUTH_ENABLE
    ESP_LOGE(TAG, "Authenticated MCP transport is required");
    return ESP_ERR_INVALID_STATE;
#endif
    ESP_RETURN_ON_FALSE(strlen(identity->auth_token) ==
                            DEVICE_AUTH_TOKEN_MAX_LEN - 1U,
                        ESP_ERR_INVALID_STATE, TAG,
                        "Device Bearer token is missing or invalid");
    s_identity = *identity;
    ESP_RETURN_ON_ERROR(esp_mcp_create(&s_mcp), TAG,
                        "MCP engine creation failed");
    ESP_RETURN_ON_ERROR(esp_mcp_set_server_info(
                            s_mcp, "Caelus CMPP Test Board",
                            "Six PWM indicators and four relay outputs", NULL,
                            NULL),
                        TAG, "MCP server metadata failed");
    const cmpp_mcp_adapter_config_t adapter_config = {
        .authenticated_transport = true,
    };
    ESP_RETURN_ON_ERROR(esp_error_from_cmpp(cmpp_mcp_adapter_create(
                            s_mcp, runtime, &adapter_config, &s_adapter)),
                        TAG, "CMPP MCP adapter failed");

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = CONFIG_CMPP_BOARD_HTTP_PORT;
    http_config.stack_size = 8192;
    const esp_mcp_mgr_config_t manager_config = {
        .transport = esp_mcp_transport_http_server,
        .config = &http_config,
        .instance = s_mcp,
    };
    ESP_RETURN_ON_ERROR(esp_mcp_mgr_init(manager_config, &s_manager), TAG,
                        "MCP manager init failed");
    ESP_RETURN_ON_ERROR(esp_mcp_mgr_set_server_auth_callback(
                            s_manager, validate_bearer_token, NULL),
                        TAG, "MCP authentication setup failed");
    ESP_RETURN_ON_ERROR(esp_mcp_mgr_start(s_manager), TAG,
                        "MCP manager start failed");
    ESP_RETURN_ON_ERROR(esp_mcp_mgr_register_endpoint(s_manager, "mcp", NULL),
                        TAG, "MCP endpoint registration failed");
    ESP_RETURN_ON_ERROR(start_mdns(), TAG, "mDNS advertisement failed");
    ESP_LOGI(TAG, "CMPP MCP endpoint: http://%s.local:%d/mcp",
             s_identity.hostname, CONFIG_CMPP_BOARD_HTTP_PORT);
    ESP_LOGI(TAG, "DNS-SD service: _cmpp._tcp.local");
    return ESP_OK;
}
