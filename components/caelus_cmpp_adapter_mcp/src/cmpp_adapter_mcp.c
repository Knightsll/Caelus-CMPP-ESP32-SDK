/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/cmpp_adapter_mcp.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cmpp/cmpp_codec_json.h"
#include "esp_err.h"
#include "esp_mcp_property.h"
#include "esp_mcp_resource.h"
#include "esp_mcp_tool.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define RESOURCE_COUNT 6U

struct cmpp_mcp_adapter {
    esp_mcp_t *mcp;
    cmpp_runtime_t *runtime;
    esp_mcp_tool_t *tool;
    esp_mcp_resource_t *resources[RESOURCE_COUNT];
    SemaphoreHandle_t state_lock;
    bool event_callback_installed;
    char *latest_result;
    char *latest_event;
};

static cmpp_mcp_adapter_t *s_active_adapter;

static char *duplicate_text(const char *text)
{
    if (!text) {
        return NULL;
    }
    const size_t length = strlen(text) + 1U;
    char *copy = malloc(length);
    if (copy) {
        memcpy(copy, text, length);
    }
    return copy;
}

static void store_json(cmpp_mcp_adapter_t *adapter, bool event,
                       const char *json)
{
    char *copy = duplicate_text(json ? json : "{}");
    if (!copy || !adapter ||
        xSemaphoreTake(adapter->state_lock, portMAX_DELAY) != pdTRUE) {
        free(copy);
        return;
    }
    char **slot = event ? &adapter->latest_event : &adapter->latest_result;
    free(*slot);
    *slot = copy;
    xSemaphoreGive(adapter->state_lock);
}

static char *load_json(cmpp_mcp_adapter_t *adapter, bool event)
{
    if (!adapter ||
        xSemaphoreTake(adapter->state_lock, portMAX_DELAY) != pdTRUE) {
        return NULL;
    }
    char *copy = duplicate_text(event ? adapter->latest_event
                                      : adapter->latest_result);
    xSemaphoreGive(adapter->state_lock);
    return copy;
}

static esp_err_t execute_callback(const esp_mcp_property_list_t *properties,
                                  esp_mcp_tool_result_t *tool_result)
{
    if (!s_active_adapter) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *command = esp_mcp_property_list_get_property_object(
        properties, "command");
    char *result_json = NULL;
    cmpp_status_t status = cmpp_mcp_dispatch_json(s_active_adapter->runtime,
                                                  command, &result_json);
    if (status != CMPP_OK) {
        cJSON *error = cJSON_CreateObject();
        if (!error) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(error, "schema", "cmpp.binding-error.v1");
        cJSON_AddStringToObject(error, "code", cmpp_status_string(status));
        result_json = cJSON_PrintUnformatted(error);
        cJSON_Delete(error);
        if (!result_json) {
            return ESP_ERR_NO_MEM;
        }
    }
    store_json(s_active_adapter, false, result_json);
    esp_err_t error = esp_mcp_tool_result_add_text(tool_result, result_json);
    if (error == ESP_OK) {
        error = esp_mcp_tool_result_set_structured_json(tool_result,
                                                        result_json);
    }
    if (error == ESP_OK) {
        cJSON *parsed = cJSON_Parse(result_json);
        cJSON *outcome = parsed
                             ? cJSON_GetObjectItemCaseSensitive(parsed, "status")
                             : NULL;
        const bool is_error = status != CMPP_OK ||
            (cJSON_IsString(outcome) &&
             strcmp(outcome->valuestring, "completed") != 0);
        error = esp_mcp_tool_result_set_is_error(tool_result, is_error);
        cJSON_Delete(parsed);
    }
    cmpp_json_free(result_json);
    return error;
}

static esp_err_t resource_result(char **out_mime_type, char **out_text,
                                 char **out_blob_base64, char *json)
{
    if (!out_mime_type || !out_text || !out_blob_base64 || !json) {
        free(json);
        return ESP_ERR_INVALID_ARG;
    }
    *out_mime_type = duplicate_text("application/json");
    *out_text = json;
    *out_blob_base64 = NULL;
    if (!*out_mime_type) {
        free(json);
        *out_text = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t read_manifest(const char *uri, char **mime, char **text,
                               char **blob, void *context)
{
    (void)uri;
    cmpp_mcp_adapter_t *adapter = context;
    char *json = NULL;
    return cmpp_json_encode_manifest(adapter->runtime, &json) == CMPP_OK
               ? resource_result(mime, text, blob, json)
               : ESP_FAIL;
}

static esp_err_t read_health(const char *uri, char **mime, char **text,
                             char **blob, void *context)
{
    (void)uri;
    cmpp_mcp_adapter_t *adapter = context;
    char *json = NULL;
    return cmpp_json_encode_health(adapter->runtime, &json) == CMPP_OK
               ? resource_result(mime, text, blob, json)
               : ESP_FAIL;
}

static esp_err_t read_state(const char *uri, char **mime, char **text,
                            char **blob, void *context)
{
    (void)uri;
    cmpp_mcp_adapter_t *adapter = context;
    cmpp_runtime_info_t info;
    if (cmpp_runtime_get_info(adapter->runtime, &info) != CMPP_OK) {
        return ESP_FAIL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *states = root ? cJSON_AddObjectToObject(root, "capabilities") : NULL;
    if (!root || !states) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "schema", "cmpp.state.v1");
    cJSON_AddStringToObject(root, "device_id", info.device_id);
    cJSON_AddNumberToObject(root, "state_version", (double)info.state_version);
    for (size_t index = 0; index < info.capability_count; ++index) {
        const cmpp_capability_descriptor_t *descriptor = NULL;
        cmpp_value_t value;
        if (cmpp_runtime_get_capability(adapter->runtime, index, &descriptor) !=
            CMPP_OK) {
            continue;
        }
        if (cmpp_runtime_observe(adapter->runtime, descriptor->capability_id,
                                 &value) != CMPP_OK) {
            cJSON_AddNullToObject(states, descriptor->capability_id);
            continue;
        }
        char *value_json = NULL;
        if (cmpp_json_encode_value(&value, &value_json) == CMPP_OK) {
            cJSON *item = cJSON_Parse(value_json);
            if (item) {
                cJSON_AddItemToObject(states, descriptor->capability_id, item);
            }
            cmpp_json_free(value_json);
        }
        cmpp_value_deinit(adapter->runtime, &value);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json ? resource_result(mime, text, blob, json) : ESP_ERR_NO_MEM;
}

static esp_err_t read_limitations(const char *uri, char **mime, char **text,
                                  char **blob, void *context)
{
    (void)uri;
    (void)context;
    return resource_result(
        mime, text, blob,
        duplicate_text(
            "{\"schema\":\"cmpp.limitations.v1\","
            "\"idempotency_scope\":\"runtime_cache\","
            "\"execution\":\"serialized\","
            "\"physical_feedback\":\"capability_defined\","
            "\"authentication\":\"required_at_transport_boundary\"}"));
}

static esp_err_t read_latest_result(const char *uri, char **mime, char **text,
                                    char **blob, void *context)
{
    (void)uri;
    return resource_result(mime, text, blob,
                           load_json((cmpp_mcp_adapter_t *)context, false));
}

static esp_err_t read_latest_event(const char *uri, char **mime, char **text,
                                   char **blob, void *context)
{
    (void)uri;
    return resource_result(mime, text, blob,
                           load_json((cmpp_mcp_adapter_t *)context, true));
}

typedef esp_err_t (*read_callback_t)(const char *, char **, char **, char **,
                                    void *);

static cmpp_status_t add_resource(cmpp_mcp_adapter_t *adapter, size_t index,
                                  const char *uri, const char *name,
                                  const char *title, const char *description,
                                  read_callback_t callback)
{
    esp_mcp_resource_t *resource = esp_mcp_resource_create(
        uri, name, title, description, "application/json", callback, adapter);
    if (!resource) {
        return CMPP_ERR_NO_MEMORY;
    }
    if (esp_mcp_add_resource(adapter->mcp, resource) != ESP_OK) {
        esp_mcp_resource_destroy(resource);
        return CMPP_ERR_INTERNAL;
    }
    adapter->resources[index] = resource;
    return CMPP_OK;
}

cmpp_status_t cmpp_mcp_adapter_create(
    esp_mcp_t *mcp, cmpp_runtime_t *runtime,
    const cmpp_mcp_adapter_config_t *config,
    cmpp_mcp_adapter_t **output)
{
    if (!mcp || !runtime || !config || !output ||
        !config->authenticated_transport || s_active_adapter) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    *output = NULL;
    cmpp_mcp_adapter_t *adapter = calloc(1U, sizeof(*adapter));
    if (!adapter) {
        return CMPP_ERR_NO_MEMORY;
    }
    adapter->mcp = mcp;
    adapter->runtime = runtime;
    adapter->state_lock = xSemaphoreCreateMutex();
    adapter->latest_result = duplicate_text("{}");
    adapter->latest_event = duplicate_text("{}");
    if (!adapter->state_lock || !adapter->latest_result ||
        !adapter->latest_event) {
        cmpp_mcp_adapter_destroy(adapter);
        return CMPP_ERR_NO_MEMORY;
    }
    adapter->tool = esp_mcp_tool_create_ex(
        CMPP_MCP_TOOL_EXECUTE, "Execute CMPP command",
        "Dispatch one transport-neutral cmpp.command.v1 envelope",
        execute_callback);
    esp_mcp_property_t *property = adapter->tool
                                       ? esp_mcp_property_create(
                                             "command",
                                             ESP_MCP_PROPERTY_TYPE_OBJECT)
                                       : NULL;
    if (!adapter->tool || !property ||
        esp_mcp_tool_add_property(adapter->tool, property) != ESP_OK ||
        esp_mcp_tool_set_output_schema_json(
            adapter->tool,
            "{\"type\":\"object\",\"required\":[\"schema\",\"status\",\"effect\"],"
            "\"properties\":{\"schema\":{\"const\":\"cmpp.command-result.v1\"},"
            "\"status\":{\"enum\":[\"rejected\",\"completed\",\"failed\"]},"
            "\"effect\":{\"enum\":[\"no_effect\",\"committed\",\"unknown\"]}}}") !=
            ESP_OK ||
        esp_mcp_tool_set_annotations_json(
            adapter->tool,
            "{\"readOnlyHint\":false,\"idempotentHint\":true,"
            "\"openWorldHint\":false}") != ESP_OK ||
        esp_mcp_add_tool(mcp, adapter->tool) != ESP_OK) {
        if (property && !adapter->tool) {
            esp_mcp_property_destroy(property);
        }
        cmpp_mcp_adapter_destroy(adapter);
        return CMPP_ERR_INTERNAL;
    }
    const struct {
        const char *uri;
        const char *name;
        const char *title;
        const char *description;
        read_callback_t callback;
    } resources[RESOURCE_COUNT] = {
        {CMPP_MCP_RESOURCE_MANIFEST, "cmpp.manifest", "CMPP Manifest",
         "Registered CMPP capabilities", read_manifest},
        {CMPP_MCP_RESOURCE_STATE, "cmpp.state", "CMPP State",
         "Current observable capability state", read_state},
        {CMPP_MCP_RESOURCE_HEALTH, "cmpp.health", "CMPP Health",
         "CMPP runtime health", read_health},
        {CMPP_MCP_RESOURCE_LIMITATIONS, "cmpp.limitations",
         "CMPP Limitations", "Runtime and transport limitations",
         read_limitations},
        {CMPP_MCP_RESOURCE_LATEST_RESULT, "cmpp.latest_result",
         "Latest CMPP Result", "Latest command result", read_latest_result},
        {CMPP_MCP_RESOURCE_LATEST_EVENT, "cmpp.latest_event",
         "Latest CMPP Event", "Latest derived event", read_latest_event},
    };
    for (size_t index = 0; index < RESOURCE_COUNT; ++index) {
        cmpp_status_t status = add_resource(
            adapter, index, resources[index].uri, resources[index].name,
            resources[index].title, resources[index].description,
            resources[index].callback);
        if (status != CMPP_OK) {
            cmpp_mcp_adapter_destroy(adapter);
            return status;
        }
    }
    if (cmpp_runtime_set_event_callback(runtime,
                                        cmpp_mcp_adapter_publish_event,
                                        adapter) != CMPP_OK) {
        cmpp_mcp_adapter_destroy(adapter);
        return CMPP_ERR_INTERNAL;
    }
    adapter->event_callback_installed = true;
    s_active_adapter = adapter;
    *output = adapter;
    return CMPP_OK;
}

void cmpp_mcp_adapter_destroy(cmpp_mcp_adapter_t *adapter)
{
    if (!adapter) {
        return;
    }
    if (s_active_adapter == adapter) {
        s_active_adapter = NULL;
    }
    if (adapter->event_callback_installed) {
        (void)cmpp_runtime_set_event_callback(adapter->runtime, NULL, NULL);
    }
    for (size_t index = 0; index < RESOURCE_COUNT; ++index) {
        if (adapter->resources[index]) {
            (void)esp_mcp_remove_resource(adapter->mcp,
                                          adapter->resources[index]);
            (void)esp_mcp_resource_destroy(adapter->resources[index]);
        }
    }
    if (adapter->tool) {
        (void)esp_mcp_remove_tool(adapter->mcp, adapter->tool);
        (void)esp_mcp_tool_destroy(adapter->tool);
    }
    if (adapter->state_lock) {
        vSemaphoreDelete(adapter->state_lock);
    }
    free(adapter->latest_result);
    free(adapter->latest_event);
    free(adapter);
}

void cmpp_mcp_adapter_publish_event(void *context,
                                    const cmpp_response_t *response)
{
    cmpp_mcp_adapter_t *adapter = context;
    if (!adapter || !response) {
        return;
    }
    char *json = NULL;
    if (cmpp_json_encode_response(response, &json) != CMPP_OK) {
        return;
    }
    store_json(adapter, true, json);
    (void)esp_mcp_notify_log_message(adapter->mcp, "info", "cmpp", json);
    cmpp_json_free(json);
}
