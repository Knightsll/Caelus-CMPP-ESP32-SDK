/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/cmpp_codec_json.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

struct cmpp_json_document {
    cJSON *json;
    cmpp_value_t value;
};

static void free_value(cmpp_value_t *value)
{
    if (!value) {
        return;
    }
    if (value->type == CMPP_VALUE_ARRAY) {
        cmpp_value_t *items = (cmpp_value_t *)value->as.array.items;
        for (size_t index = 0; index < value->as.array.count; ++index) {
            free_value(&items[index]);
        }
        free(items);
    } else if (value->type == CMPP_VALUE_OBJECT) {
        cmpp_object_entry_t *entries =
            (cmpp_object_entry_t *)value->as.object.entries;
        for (size_t index = 0; index < value->as.object.count; ++index) {
            free_value((cmpp_value_t *)entries[index].value);
            free((void *)entries[index].value);
        }
        free(entries);
    }
    memset(value, 0, sizeof(*value));
}

static cmpp_status_t from_json(cJSON *json, cmpp_value_t *value)
{
    memset(value, 0, sizeof(*value));
    if (cJSON_IsNull(json)) {
        value->type = CMPP_VALUE_NULL;
    } else if (cJSON_IsBool(json)) {
        value->type = CMPP_VALUE_BOOL;
        value->as.boolean = cJSON_IsTrue(json);
    } else if (cJSON_IsNumber(json)) {
        const double number = json->valuedouble;
        const int64_t integer = (int64_t)number;
        if ((double)integer == number) {
            value->type = CMPP_VALUE_INT;
            value->as.integer = integer;
        } else {
            value->type = CMPP_VALUE_DOUBLE;
            value->as.number = number;
        }
    } else if (cJSON_IsString(json) && json->valuestring) {
        value->type = CMPP_VALUE_STRING;
        value->as.string = json->valuestring;
    } else if (cJSON_IsArray(json)) {
        const int count = cJSON_GetArraySize(json);
        cmpp_value_t *items = count > 0 ? calloc((size_t)count, sizeof(*items))
                                       : NULL;
        if (count > 0 && !items) {
            return CMPP_ERR_NO_MEMORY;
        }
        value->type = CMPP_VALUE_ARRAY;
        value->as.array.items = items;
        value->as.array.count = (size_t)count;
        for (int index = 0; index < count; ++index) {
            cmpp_status_t status = from_json(cJSON_GetArrayItem(json, index),
                                             &items[index]);
            if (status != CMPP_OK) {
                free_value(value);
                return status;
            }
        }
    } else if (cJSON_IsObject(json)) {
        size_t count = 0U;
        for (cJSON *child = json->child; child; child = child->next) {
            ++count;
        }
        cmpp_object_entry_t *entries =
            count > 0U ? calloc(count, sizeof(*entries)) : NULL;
        if (count > 0U && !entries) {
            return CMPP_ERR_NO_MEMORY;
        }
        value->type = CMPP_VALUE_OBJECT;
        value->as.object.entries = entries;
        value->as.object.count = count;
        size_t index = 0U;
        for (cJSON *child = json->child; child; child = child->next, ++index) {
            for (size_t prior = 0U; prior < index; ++prior) {
                if (entries[prior].key && child->string &&
                    strcmp(entries[prior].key, child->string) == 0) {
                    free_value(value);
                    return CMPP_ERR_INVALID_ARGUMENT;
                }
            }
            entries[index].key = child->string;
            cmpp_value_t *item = calloc(1U, sizeof(*item));
            entries[index].value = item;
            if (!item) {
                free_value(value);
                return CMPP_ERR_NO_MEMORY;
            }
            cmpp_status_t status = from_json(child, item);
            if (status != CMPP_OK) {
                free_value(value);
                return status;
            }
        }
    } else {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    return CMPP_OK;
}

static cJSON *to_json(const cmpp_value_t *value)
{
    if (!value) {
        return NULL;
    }
    switch (value->type) {
    case CMPP_VALUE_NULL:
        return cJSON_CreateNull();
    case CMPP_VALUE_BOOL:
        return cJSON_CreateBool(value->as.boolean);
    case CMPP_VALUE_INT:
        return cJSON_CreateNumber((double)value->as.integer);
    case CMPP_VALUE_DOUBLE:
        return cJSON_CreateNumber(value->as.number);
    case CMPP_VALUE_STRING:
        return value->as.string ? cJSON_CreateString(value->as.string) : NULL;
    case CMPP_VALUE_ARRAY: {
        cJSON *array = cJSON_CreateArray();
        if (!array) {
            return NULL;
        }
        for (size_t index = 0; index < value->as.array.count; ++index) {
            cJSON *item = to_json(&value->as.array.items[index]);
            if (!item || !cJSON_AddItemToArray(array, item)) {
                cJSON_Delete(item);
                cJSON_Delete(array);
                return NULL;
            }
        }
        return array;
    }
    case CMPP_VALUE_OBJECT: {
        cJSON *object = cJSON_CreateObject();
        if (!object) {
            return NULL;
        }
        for (size_t index = 0; index < value->as.object.count; ++index) {
            const cmpp_object_entry_t *entry = &value->as.object.entries[index];
            cJSON *item = entry->key ? to_json(entry->value) : NULL;
            if (!item || !cJSON_AddItemToObject(object, entry->key, item)) {
                cJSON_Delete(item);
                cJSON_Delete(object);
                return NULL;
            }
        }
        return object;
    }
    }
    return NULL;
}

static const char *json_string(cJSON *object, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool json_bool(cJSON *object, const char *key, bool default_value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : default_value;
}

static bool json_u64(cJSON *object, const char *key, uint64_t *value,
                     bool required)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item && !required) {
        *value = 0U;
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0) {
        return false;
    }
    const uint64_t number = (uint64_t)item->valuedouble;
    if ((double)number != item->valuedouble) {
        return false;
    }
    *value = number;
    return true;
}

cmpp_status_t cmpp_json_parse_value(const char *json,
                                    cmpp_json_document_t **output,
                                    const cmpp_value_t **value)
{
    if (!json || !output || !value) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    *output = NULL;
    *value = NULL;
    cmpp_json_document_t *document = calloc(1U, sizeof(*document));
    if (!document) {
        return CMPP_ERR_NO_MEMORY;
    }
    document->json = cJSON_Parse(json);
    if (!document->json) {
        free(document);
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    cmpp_status_t status = from_json(document->json, &document->value);
    if (status != CMPP_OK) {
        cmpp_json_document_destroy(document);
        return status;
    }
    *output = document;
    *value = &document->value;
    return CMPP_OK;
}

cmpp_status_t cmpp_json_decode_request(const char *json,
                                       cmpp_json_document_t **output,
                                       cmpp_request_t *request)
{
    if (!json || !output || !request) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    *output = NULL;
    memset(request, 0, sizeof(*request));
    cmpp_json_document_t *document = calloc(1U, sizeof(*document));
    if (!document) {
        return CMPP_ERR_NO_MEMORY;
    }
    document->json = cJSON_Parse(json);
    cJSON *root = document->json;
    cJSON *target = root ? cJSON_GetObjectItemCaseSensitive(root, "target") : NULL;
    cJSON *arguments = root ? cJSON_GetObjectItemCaseSensitive(root, "arguments") : NULL;
    uint64_t ttl = 0U;
    uint64_t absolute_deadline = 0U;
    cmpp_status_t status = CMPP_ERR_INVALID_ARGUMENT;
    if (!cJSON_IsObject(root) || !cJSON_IsObject(target) ||
        !cJSON_IsObject(arguments) ||
        !json_u64(root, "ttl_ms", &ttl, true) || ttl > UINT32_MAX ||
        !json_u64(root, "absolute_deadline_ms", &absolute_deadline, false)) {
        goto fail;
    }
    const char *safety = json_string(root, "safety_class");
    if (!cmpp_safety_class_parse(safety, &request->safety_class)) {
        goto fail;
    }
    status = from_json(arguments, &document->value);
    if (status != CMPP_OK) {
        goto fail;
    }
    request->schema = json_string(root, "schema");
    request->protocol_version = json_string(root, "protocol_version");
    request->command_id = json_string(root, "command_id");
    request->trace_id = json_string(root, "trace_id");
    request->device_id = json_string(target, "device_id");
    request->capability_id = json_string(target, "capability_id");
    request->command_type = json_string(root, "command_type");
    request->requested_by = json_string(root, "requested_by");
    request->reason = json_string(root, "reason");
    request->idempotency_key = json_string(root, "idempotency_key");
    request->arguments = &document->value;
    request->ttl_ms = (uint32_t)ttl;
    request->absolute_deadline_ms = absolute_deadline;
    request->dry_run = json_bool(root, "dry_run", false);
    request->requires_ack = json_bool(root, "requires_ack", false);
    *output = document;
    return CMPP_OK;

fail:
    cmpp_json_document_destroy(document);
    return status;
}

void cmpp_json_document_destroy(cmpp_json_document_t *document)
{
    if (document) {
        free_value(&document->value);
        cJSON_Delete(document->json);
        free(document);
    }
}

static cmpp_status_t print_json(cJSON *object, char **json)
{
    if (!object || !json) {
        cJSON_Delete(object);
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    *json = cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    return *json ? CMPP_OK : CMPP_ERR_NO_MEMORY;
}

cmpp_status_t cmpp_json_encode_value(const cmpp_value_t *value, char **json)
{
    if (!value || !json) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    cJSON *object = to_json(value);
    return object ? print_json(object, json) : CMPP_ERR_NO_MEMORY;
}

cmpp_status_t cmpp_json_encode_response(const cmpp_response_t *response,
                                        char **json)
{
    if (!response || !json) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return CMPP_ERR_NO_MEMORY;
    }
    cJSON_AddStringToObject(root, "schema", "cmpp.command-result.v1");
    cJSON_AddStringToObject(root, "command_id", response->command_id);
    cJSON_AddStringToObject(root, "trace_id", response->trace_id);
    cJSON_AddStringToObject(root, "status",
                           cmpp_outcome_string(response->outcome));
    cJSON_AddStringToObject(root, "effect",
                           cmpp_effect_string(response->effect));
    cJSON_AddNumberToObject(root, "state_version",
                            (double)response->state_version);
    cJSON_AddBoolToObject(root, "idempotent_replay",
                          response->from_idempotency_cache);
    if (response->has_data) {
        cJSON *data = to_json(&response->data);
        if (!data) {
            cJSON_Delete(root);
            return CMPP_ERR_NO_MEMORY;
        }
        cJSON_AddItemToObject(root, "result", data);
    }
    if (response->error_code[0]) {
        cJSON *error = cJSON_AddObjectToObject(root, "error");
        cJSON_AddStringToObject(error, "code", response->error_code);
        cJSON_AddStringToObject(error, "message", response->error_message);
    }
    return print_json(root, json);
}

cmpp_status_t cmpp_json_encode_manifest(cmpp_runtime_t *runtime, char **json)
{
    if (!runtime || !json) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    cmpp_runtime_info_t info;
    if (cmpp_runtime_get_info(runtime, &info) != CMPP_OK) {
        return CMPP_ERR_INTERNAL;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *capabilities = root ? cJSON_AddArrayToObject(root, "capabilities") : NULL;
    if (!root || !capabilities) {
        cJSON_Delete(root);
        return CMPP_ERR_NO_MEMORY;
    }
    cJSON_AddStringToObject(root, "schema", "cmpp.manifest.v1");
    cJSON_AddStringToObject(root, "protocol_version", CMPP_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "device_id", info.device_id);
    cJSON_AddStringToObject(root, "profile_id", info.profile_id);
    cJSON_AddNumberToObject(root, "state_version", (double)info.state_version);
    for (size_t index = 0; index < info.capability_count; ++index) {
        const cmpp_capability_descriptor_t *descriptor = NULL;
        if (cmpp_runtime_get_capability(runtime, index, &descriptor) != CMPP_OK) {
            cJSON_Delete(root);
            return CMPP_ERR_INTERNAL;
        }
        cJSON *capability = cJSON_CreateObject();
        cJSON *commands = capability
                              ? cJSON_AddArrayToObject(capability, "command_types")
                              : NULL;
        if (!capability || !commands) {
            cJSON_Delete(capability);
            cJSON_Delete(root);
            return CMPP_ERR_NO_MEMORY;
        }
        cJSON_AddStringToObject(capability, "capability_id",
                               descriptor->capability_id);
        cJSON_AddStringToObject(capability, "display_name",
                               descriptor->display_name ? descriptor->display_name
                                                        : descriptor->capability_id);
        cJSON_AddStringToObject(capability, "version", descriptor->version);
        if (descriptor->description) {
            cJSON_AddStringToObject(capability, "description",
                                   descriptor->description);
        }
        if (descriptor->arguments_schema_json) {
            cJSON *schema = cJSON_Parse(descriptor->arguments_schema_json);
            if (schema) {
                cJSON_AddItemToObject(capability, "arguments_schema", schema);
            }
        }
        if (descriptor->state_schema_json) {
            cJSON *schema = cJSON_Parse(descriptor->state_schema_json);
            if (schema) {
                cJSON_AddItemToObject(capability, "state_schema", schema);
            }
        }
        for (size_t command = 0; command < descriptor->command_type_count;
             ++command) {
            cJSON_AddItemToArray(
                commands,
                cJSON_CreateString(descriptor->command_types[command]));
        }
        cJSON_AddNumberToObject(capability, "allowed_safety_mask",
                               descriptor->allowed_safety_mask);
        cJSON_AddBoolToObject(capability, "requires_ack",
                              descriptor->requires_ack);
        cJSON_AddBoolToObject(capability, "supports_dry_run",
                              descriptor->supports_dry_run);
        cJSON_AddItemToArray(capabilities, capability);
    }
    return print_json(root, json);
}

cmpp_status_t cmpp_json_encode_health(cmpp_runtime_t *runtime, char **json)
{
    if (!runtime || !json) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    cmpp_runtime_info_t info;
    if (cmpp_runtime_get_info(runtime, &info) != CMPP_OK) {
        return CMPP_ERR_INTERNAL;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return CMPP_ERR_NO_MEMORY;
    }
    cJSON_AddStringToObject(root, "schema", "cmpp.health.v1");
    cJSON_AddStringToObject(root, "status", "healthy");
    cJSON_AddStringToObject(root, "device_id", info.device_id);
    cJSON_AddNumberToObject(root, "state_version", (double)info.state_version);
    cJSON_AddNumberToObject(root, "capability_count",
                            (double)info.capability_count);
    return print_json(root, json);
}

void cmpp_json_free(char *json)
{
    cJSON_free(json);
}
