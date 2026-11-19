/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/cmpp_sdk_esp32.h"

#include <stdlib.h>
#include <string.h>

#include "cmpp/cmpp_platform_esp32.h"
#include "cmpp_device_server.h"
#include "cmpp_mqtt_client.h"
#include "device_identity.h"
#include "esp_check.h"
#include "esp_log.h"
#include "network.h"
#include "nvs_flash.h"

#define SDK_SERVER_NAME_MAX 64U
#define SDK_SERVER_DESCRIPTION_MAX 160U
#define SDK_DEVICE_TYPE_MAX 64U

struct cmpp_esp32_sdk {
    cmpp_esp32_platform_t *platform;
    cmpp_runtime_t *runtime;
    device_identity_t identity;
    char server_name[SDK_SERVER_NAME_MAX];
    char server_description[SDK_SERVER_DESCRIPTION_MAX];
    char device_type[SDK_DEVICE_TYPE_MAX];
    bool started;
};

static const char *TAG = "cmpp_esp32_sdk";

static bool copy_required(char *output, size_t output_size,
                          const char *input)
{
    if (!output || output_size == 0U || !input || input[0] == '\0' ||
        strlen(input) >= output_size) {
        return false;
    }
    strlcpy(output, input, output_size);
    return true;
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t esp_error_from_cmpp(cmpp_status_t status)
{
    if (status == CMPP_OK) {
        return ESP_OK;
    }
    if (status == CMPP_ERR_NO_MEMORY) {
        return ESP_ERR_NO_MEM;
    }
    if (status == CMPP_ERR_INVALID_ARGUMENT) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_FAIL;
}

esp_err_t cmpp_esp32_sdk_create(const cmpp_esp32_sdk_config_t *config,
                                cmpp_esp32_sdk_t **output)
{
    if (!config || !output || !config->profile_id ||
        config->max_capabilities == 0U ||
        config->idempotency_cache_size == 0U || config->max_ttl_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *output = NULL;
    cmpp_esp32_sdk_t *sdk = calloc(1U, sizeof(*sdk));
    if (!sdk) {
        return ESP_ERR_NO_MEM;
    }
    if (!copy_required(sdk->server_name, sizeof(sdk->server_name),
                       config->server_name) ||
        !copy_required(sdk->server_description,
                       sizeof(sdk->server_description),
                       config->server_description) ||
        !copy_required(sdk->device_type, sizeof(sdk->device_type),
                       config->device_type)) {
        free(sdk);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = init_nvs();
    if (err == ESP_OK) {
        err = esp_error_from_cmpp(
            cmpp_esp32_platform_create(NULL, &sdk->platform));
    }
    if (err == ESP_OK) {
        err = device_identity_load(&sdk->identity);
    }
    if (err == ESP_OK) {
        const cmpp_runtime_config_t runtime_config = {
            .device_id = sdk->identity.device_uid,
            .profile_id = config->profile_id,
            .max_capabilities = config->max_capabilities,
            .idempotency_cache_size = config->idempotency_cache_size,
            .max_ttl_ms = config->max_ttl_ms,
            .persist_state_version = config->persist_state_version,
            .event_callback = config->event_callback,
            .event_context = config->event_context,
        };
        err = esp_error_from_cmpp(cmpp_runtime_create(
            cmpp_esp32_platform_ops(sdk->platform), &runtime_config,
            &sdk->runtime));
    }
    if (err != ESP_OK) {
        if (sdk->runtime) {
            cmpp_runtime_destroy(sdk->runtime);
        }
        if (sdk->platform) {
            cmpp_esp32_platform_destroy(sdk->platform);
        }
        memset(&sdk->identity, 0, sizeof(sdk->identity));
        free(sdk);
        return err;
    }
    *output = sdk;
    return ESP_OK;
}

cmpp_runtime_t *cmpp_esp32_sdk_runtime(cmpp_esp32_sdk_t *sdk)
{
    return sdk ? sdk->runtime : NULL;
}

const char *cmpp_esp32_sdk_device_uid(const cmpp_esp32_sdk_t *sdk)
{
    return sdk ? sdk->identity.device_uid : NULL;
}

esp_err_t cmpp_esp32_sdk_start(cmpp_esp32_sdk_t *sdk)
{
    if (!sdk || !sdk->runtime || sdk->started) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(network_connect(), TAG, "Network setup failed");
    ESP_RETURN_ON_ERROR(device_identity_load(&sdk->identity), TAG,
                        "Device identity reload failed");

    const cmpp_device_server_config_t server_config = {
        .server_name = sdk->server_name,
        .server_description = sdk->server_description,
        .device_type = sdk->device_type,
    };
    ESP_RETURN_ON_ERROR(cmpp_device_server_start(
                            &sdk->identity, sdk->runtime, &server_config),
                        TAG, "MCP server start failed");
    const esp_err_t mqtt_err =
        cmpp_mqtt_client_start(&sdk->identity, sdk->runtime);
    if (mqtt_err != ESP_OK) {
        ESP_LOGW(TAG, "Optional MQTT transport did not start: %s",
                 esp_err_to_name(mqtt_err));
    }
    sdk->started = true;
    ESP_LOGI(TAG, "Device UID: %s", sdk->identity.device_uid);
    ESP_LOGI(TAG, "Ready: http://%s.local:%d/mcp", sdk->identity.hostname,
             CONFIG_CMPP_SDK_HTTP_PORT);
    return ESP_OK;
}

esp_err_t cmpp_esp32_sdk_destroy(cmpp_esp32_sdk_t *sdk)
{
    if (!sdk) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sdk->started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sdk->runtime) {
        cmpp_runtime_destroy(sdk->runtime);
    }
    if (sdk->platform) {
        cmpp_esp32_platform_destroy(sdk->platform);
    }
    memset(sdk, 0, sizeof(*sdk));
    free(sdk);
    return ESP_OK;
}
