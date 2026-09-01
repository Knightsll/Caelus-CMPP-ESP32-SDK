/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/cmpp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define STATE_VERSION_KEY "cmpp.state_ver"

typedef struct {
    const cmpp_capability_descriptor_t *descriptor;
    cmpp_capability_ops_t ops;
    void *context;
} capability_record_t;

typedef struct {
    bool occupied;
    uint64_t request_hash;
    char command_id[CMPP_ID_MAX + 1U];
    char idempotency_key[CMPP_ID_MAX + 1U];
    cmpp_response_t response;
} cache_entry_t;

struct cmpp_runtime {
    cmpp_platform_ops_t platform;
    cmpp_runtime_config_t config;
    char device_id[CMPP_ID_MAX + 1U];
    char profile_id[CMPP_ID_MAX + 1U];
    capability_record_t *capabilities;
    size_t capability_count;
    cache_entry_t *cache;
    size_t next_cache;
    uint64_t state_version;
    cmpp_lock_handle_t metadata_lock;
    cmpp_lock_handle_t execution_lock;
};

static void *rt_alloc(cmpp_runtime_t *runtime, size_t size)
{
    return runtime->platform.allocate(runtime->platform.context, size);
}

static void rt_free(cmpp_runtime_t *runtime, void *memory)
{
    if (memory) {
        runtime->platform.deallocate(runtime->platform.context, memory);
    }
}

static bool bounded_text(const char *text, size_t maximum, bool allow_empty)
{
    if (!text) {
        return false;
    }
    size_t length = 0U;
    while (length <= maximum && text[length] != '\0') {
        ++length;
    }
    return length <= maximum && (allow_empty || length > 0U);
}

static bool valid_identifier(const char *text)
{
    if (!bounded_text(text, CMPP_ID_MAX, false)) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor;
         ++cursor) {
        const bool valid = (*cursor >= 'a' && *cursor <= 'z') ||
                           (*cursor >= 'A' && *cursor <= 'Z') ||
                           (*cursor >= '0' && *cursor <= '9') ||
                           *cursor == '.' || *cursor == '_' ||
                           *cursor == '-' || *cursor == ':';
        if (!valid) {
            return false;
        }
    }
    return true;
}

static void copy_text(char *destination, size_t size, const char *source)
{
    if (size == 0U) {
        return;
    }
    if (!source) {
        destination[0] = '\0';
        return;
    }
    (void)snprintf(destination, size, "%s", source);
}

static void response_error(cmpp_response_t *response, const char *code,
                           const char *message)
{
    copy_text(response->error_code, sizeof(response->error_code), code);
    copy_text(response->error_message, sizeof(response->error_message), message);
}

static cmpp_status_t clone_value(cmpp_runtime_t *runtime,
                                 const cmpp_value_t *source,
                                 cmpp_value_t *destination);

static void dispose_value(cmpp_runtime_t *runtime, cmpp_value_t *value)
{
    if (!value) {
        return;
    }
    if (value->type == CMPP_VALUE_STRING) {
        rt_free(runtime, (void *)value->as.string);
    } else if (value->type == CMPP_VALUE_ARRAY) {
        cmpp_value_t *items = (cmpp_value_t *)value->as.array.items;
        for (size_t index = 0; index < value->as.array.count; ++index) {
            dispose_value(runtime, &items[index]);
        }
        rt_free(runtime, items);
    } else if (value->type == CMPP_VALUE_OBJECT) {
        cmpp_object_entry_t *entries =
            (cmpp_object_entry_t *)value->as.object.entries;
        for (size_t index = 0; index < value->as.object.count; ++index) {
            rt_free(runtime, (void *)entries[index].key);
            dispose_value(runtime, (cmpp_value_t *)entries[index].value);
            rt_free(runtime, (void *)entries[index].value);
        }
        rt_free(runtime, entries);
    }
    memset(value, 0, sizeof(*value));
}

static char *clone_string(cmpp_runtime_t *runtime, const char *source)
{
    if (!source) {
        return NULL;
    }
    const size_t length = strlen(source) + 1U;
    char *copy = rt_alloc(runtime, length);
    if (copy) {
        memcpy(copy, source, length);
    }
    return copy;
}

static cmpp_status_t clone_value(cmpp_runtime_t *runtime,
                                 const cmpp_value_t *source,
                                 cmpp_value_t *destination)
{
    if (!source || !destination || source->type > CMPP_VALUE_OBJECT) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    memset(destination, 0, sizeof(*destination));
    destination->type = source->type;
    switch (source->type) {
    case CMPP_VALUE_NULL:
        return CMPP_OK;
    case CMPP_VALUE_BOOL:
        destination->as.boolean = source->as.boolean;
        return CMPP_OK;
    case CMPP_VALUE_INT:
        destination->as.integer = source->as.integer;
        return CMPP_OK;
    case CMPP_VALUE_DOUBLE:
        if (!isfinite(source->as.number)) {
            return CMPP_ERR_INVALID_ARGUMENT;
        }
        destination->as.number = source->as.number;
        return CMPP_OK;
    case CMPP_VALUE_STRING:
        if (!source->as.string) {
            return CMPP_ERR_INVALID_ARGUMENT;
        }
        destination->as.string = clone_string(runtime, source->as.string);
        return destination->as.string ? CMPP_OK : CMPP_ERR_NO_MEMORY;
    case CMPP_VALUE_ARRAY: {
        if (source->as.array.count > 0U && !source->as.array.items) {
            return CMPP_ERR_INVALID_ARGUMENT;
        }
        cmpp_value_t *items = NULL;
        if (source->as.array.count > 0U) {
            items = rt_alloc(runtime,
                             source->as.array.count * sizeof(*items));
            if (!items) {
                return CMPP_ERR_NO_MEMORY;
            }
            memset(items, 0, source->as.array.count * sizeof(*items));
        }
        destination->as.array.items = items;
        destination->as.array.count = source->as.array.count;
        for (size_t index = 0; index < source->as.array.count; ++index) {
            cmpp_status_t status = clone_value(runtime,
                                               &source->as.array.items[index],
                                               &items[index]);
            if (status != CMPP_OK) {
                dispose_value(runtime, destination);
                return status;
            }
        }
        return CMPP_OK;
    }
    case CMPP_VALUE_OBJECT: {
        if (source->as.object.count > 0U && !source->as.object.entries) {
            return CMPP_ERR_INVALID_ARGUMENT;
        }
        cmpp_object_entry_t *entries = NULL;
        if (source->as.object.count > 0U) {
            entries = rt_alloc(runtime,
                               source->as.object.count * sizeof(*entries));
            if (!entries) {
                return CMPP_ERR_NO_MEMORY;
            }
            memset(entries, 0,
                   source->as.object.count * sizeof(*entries));
        }
        destination->as.object.entries = entries;
        destination->as.object.count = source->as.object.count;
        for (size_t index = 0; index < source->as.object.count; ++index) {
            const cmpp_object_entry_t *input =
                &source->as.object.entries[index];
            if (!input->key || !input->value) {
                dispose_value(runtime, destination);
                return CMPP_ERR_INVALID_ARGUMENT;
            }
            entries[index].key = clone_string(runtime, input->key);
            cmpp_value_t *child = rt_alloc(runtime, sizeof(*child));
            entries[index].value = child;
            if (!entries[index].key || !child) {
                dispose_value(runtime, destination);
                return CMPP_ERR_NO_MEMORY;
            }
            memset(child, 0, sizeof(*child));
            cmpp_status_t status = clone_value(runtime, input->value, child);
            if (status != CMPP_OK) {
                dispose_value(runtime, destination);
                return status;
            }
        }
        return CMPP_OK;
    }
    }
    return CMPP_ERR_INVALID_ARGUMENT;
}

void cmpp_response_deinit(cmpp_runtime_t *runtime, cmpp_response_t *response)
{
    if (!runtime || !response) {
        return;
    }
    if (response->has_data) {
        dispose_value(runtime, &response->data);
    }
    memset(response, 0, sizeof(*response));
}

void cmpp_value_deinit(cmpp_runtime_t *runtime, cmpp_value_t *value)
{
    if (runtime && value) {
        dispose_value(runtime, value);
    }
}

static cmpp_status_t clone_response(cmpp_runtime_t *runtime,
                                    const cmpp_response_t *source,
                                    cmpp_response_t *destination)
{
    *destination = *source;
    destination->has_data = false;
    memset(&destination->data, 0, sizeof(destination->data));
    if (source->has_data) {
        cmpp_status_t status = clone_value(runtime, &source->data,
                                           &destination->data);
        if (status != CMPP_OK) {
            memset(destination, 0, sizeof(*destination));
            return status;
        }
        destination->has_data = true;
    }
    return CMPP_OK;
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t hash_text(uint64_t hash, const char *text)
{
    if (!text) {
        const uint8_t marker = 0xffU;
        return hash_bytes(hash, &marker, sizeof(marker));
    }
    return hash_bytes(hash, text, strlen(text) + 1U);
}

static uint64_t hash_value(uint64_t hash, const cmpp_value_t *value)
{
    if (!value) {
        return hash_text(hash, NULL);
    }
    hash = hash_bytes(hash, &value->type, sizeof(value->type));
    switch (value->type) {
    case CMPP_VALUE_BOOL:
        return hash_bytes(hash, &value->as.boolean, sizeof(value->as.boolean));
    case CMPP_VALUE_INT:
        return hash_bytes(hash, &value->as.integer, sizeof(value->as.integer));
    case CMPP_VALUE_DOUBLE:
        return hash_bytes(hash, &value->as.number, sizeof(value->as.number));
    case CMPP_VALUE_STRING:
        return hash_text(hash, value->as.string);
    case CMPP_VALUE_ARRAY:
        for (size_t index = 0; index < value->as.array.count; ++index) {
            hash = hash_value(hash, &value->as.array.items[index]);
        }
        return hash;
    case CMPP_VALUE_OBJECT:
        {
        uint64_t sum = 0U;
        uint64_t mixed = 0U;
        for (size_t index = 0; index < value->as.object.count; ++index) {
            uint64_t entry_hash = UINT64_C(1469598103934665603);
            entry_hash = hash_text(entry_hash,
                                   value->as.object.entries[index].key);
            entry_hash = hash_value(entry_hash,
                                    value->as.object.entries[index].value);
            sum += entry_hash;
            mixed ^= (entry_hash << 17U) | (entry_hash >> 47U);
        }
        hash = hash_bytes(hash, &value->as.object.count,
                          sizeof(value->as.object.count));
        hash = hash_bytes(hash, &sum, sizeof(sum));
        return hash_bytes(hash, &mixed, sizeof(mixed));
        }
    case CMPP_VALUE_NULL:
    default:
        return hash;
    }
}

static uint64_t request_hash(const cmpp_request_t *request)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_text(hash, request->schema);
    hash = hash_text(hash, request->protocol_version);
    hash = hash_text(hash, request->command_id);
    hash = hash_text(hash, request->trace_id);
    hash = hash_text(hash, request->device_id);
    hash = hash_text(hash, request->capability_id);
    hash = hash_text(hash, request->command_type);
    hash = hash_text(hash, request->requested_by);
    hash = hash_text(hash, request->reason);
    hash = hash_text(hash, request->idempotency_key);
    hash = hash_bytes(hash, &request->safety_class,
                      sizeof(request->safety_class));
    hash = hash_bytes(hash, &request->ttl_ms, sizeof(request->ttl_ms));
    hash = hash_bytes(hash, &request->absolute_deadline_ms,
                      sizeof(request->absolute_deadline_ms));
    hash = hash_bytes(hash, &request->dry_run, sizeof(request->dry_run));
    hash = hash_bytes(hash, &request->requires_ack,
                      sizeof(request->requires_ack));
    return hash_value(hash, request->arguments);
}

static capability_record_t *find_capability(cmpp_runtime_t *runtime,
                                             const char *capability_id)
{
    for (size_t index = 0; index < runtime->capability_count; ++index) {
        if (strcmp(runtime->capabilities[index].descriptor->capability_id,
                   capability_id) == 0) {
            return &runtime->capabilities[index];
        }
    }
    return NULL;
}

static uint64_t state_version_snapshot(cmpp_runtime_t *runtime)
{
    uint64_t version = 0U;
    if (runtime->platform.lock_acquire(runtime->platform.context,
                                       runtime->metadata_lock,
                                       CMPP_WAIT_FOREVER)) {
        version = runtime->state_version;
        runtime->platform.lock_release(runtime->platform.context,
                                       runtime->metadata_lock);
    }
    return version;
}

static bool supports_command(const capability_record_t *record,
                             const char *command_type)
{
    for (size_t index = 0;
         index < record->descriptor->command_type_count; ++index) {
        if (strcmp(record->descriptor->command_types[index], command_type) == 0) {
            return true;
        }
    }
    return false;
}

static cmpp_status_t validate_envelope(cmpp_runtime_t *runtime,
                                       const cmpp_request_t *request,
                                       capability_record_t **record,
                                       const char **error)
{
    if (!request || !record || !error) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (!request->schema || strcmp(request->schema, CMPP_COMMAND_SCHEMA) != 0 ||
        !request->protocol_version ||
        strcmp(request->protocol_version, CMPP_PROTOCOL_VERSION) != 0 ||
        !valid_identifier(request->command_id) ||
        !valid_identifier(request->trace_id) ||
        !valid_identifier(request->device_id) ||
        !valid_identifier(request->capability_id) ||
        !valid_identifier(request->command_type) ||
        !valid_identifier(request->idempotency_key) ||
        !bounded_text(request->requested_by, CMPP_ID_MAX, false) ||
        !bounded_text(request->reason, CMPP_ERROR_MESSAGE_MAX, false) ||
        !request->arguments || request->arguments->type != CMPP_VALUE_OBJECT ||
        request->safety_class >= CMPP_SAFETY_CLASS_COUNT ||
        request->ttl_ms == 0U ||
        request->ttl_ms > runtime->config.max_ttl_ms) {
        *error = "invalid_envelope";
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(request->device_id, runtime->device_id) != 0) {
        *error = "wrong_device_target";
        return CMPP_ERR_NOT_FOUND;
    }
    *record = find_capability(runtime, request->capability_id);
    if (!*record || !supports_command(*record, request->command_type)) {
        *error = "unsupported_capability_or_command";
        return CMPP_ERR_NOT_SUPPORTED;
    }
    const cmpp_capability_descriptor_t *descriptor = (*record)->descriptor;
    if ((descriptor->allowed_safety_mask &
         CMPP_SAFETY_MASK(request->safety_class)) == 0U ||
        (descriptor->requires_ack && !request->requires_ack) ||
        (request->safety_class >= CMPP_SAFETY_IRREVERSIBLE &&
         !request->requires_ack) ||
        (request->dry_run && !descriptor->supports_dry_run)) {
        *error = "safety_metadata_rejected";
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    return CMPP_OK;
}

typedef enum {
    CACHE_MISS = 0,
    CACHE_MATCH,
    CACHE_COLLISION,
    CACHE_ERROR,
} cache_lookup_t;

static cache_lookup_t cache_lookup(cmpp_runtime_t *runtime,
                                   const cmpp_request_t *request,
                                   uint64_t hash,
                                   cmpp_response_t *response)
{
    for (size_t index = 0; index < runtime->config.idempotency_cache_size;
         ++index) {
        cache_entry_t *entry = &runtime->cache[index];
        if (!entry->occupied) {
            continue;
        }
        const bool command_match =
            strcmp(entry->command_id, request->command_id) == 0;
        const bool key_match =
            strcmp(entry->idempotency_key, request->idempotency_key) == 0;
        if (!command_match && !key_match) {
            continue;
        }
        if (entry->request_hash != hash || !command_match || !key_match) {
            return CACHE_COLLISION;
        }
        if (clone_response(runtime, &entry->response, response) != CMPP_OK) {
            return CACHE_ERROR;
        }
        response->from_idempotency_cache = true;
        return CACHE_MATCH;
    }
    return CACHE_MISS;
}

static cmpp_status_t cache_store(cmpp_runtime_t *runtime,
                                 const cmpp_request_t *request,
                                 uint64_t hash,
                                 cmpp_response_t *response)
{
    if (runtime->config.idempotency_cache_size == 0U) {
        return CMPP_OK;
    }
    cache_entry_t *entry = &runtime->cache[runtime->next_cache];
    if (entry->occupied) {
        cmpp_response_deinit(runtime, &entry->response);
    }
    memset(entry, 0, sizeof(*entry));
    entry->response = *response;
    response->has_data = false;
    memset(&response->data, 0, sizeof(response->data));
    entry->occupied = true;
    entry->request_hash = hash;
    copy_text(entry->command_id, sizeof(entry->command_id), request->command_id);
    copy_text(entry->idempotency_key, sizeof(entry->idempotency_key),
              request->idempotency_key);
    runtime->next_cache =
        (runtime->next_cache + 1U) % runtime->config.idempotency_cache_size;
    cmpp_response_t caller_response;
    cmpp_status_t status = clone_response(runtime, &entry->response,
                                          &caller_response);
    if (status == CMPP_OK) {
        *response = caller_response;
    }
    return status;
}

static uint64_t deadline_for(cmpp_runtime_t *runtime,
                             const cmpp_request_t *request,
                             uint64_t received_at)
{
    uint64_t deadline = received_at + request->ttl_ms;
    if (deadline < received_at) {
        deadline = UINT64_MAX;
    }
    if (request->absolute_deadline_ms != 0U &&
        request->absolute_deadline_ms < deadline) {
        deadline = request->absolute_deadline_ms;
    }
    (void)runtime;
    return deadline;
}

static bool deadline_remaining(cmpp_runtime_t *runtime, uint64_t deadline,
                               uint32_t *remaining)
{
    const uint64_t now = runtime->platform.monotonic_ms(
        runtime->platform.context);
    if (now >= deadline) {
        *remaining = 0U;
        return false;
    }
    const uint64_t delta = deadline - now;
    *remaining = delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
    return true;
}

static void rejection(cmpp_response_t *response, const cmpp_request_t *request,
                      uint64_t state_version, const char *code,
                      const char *message)
{
    memset(response, 0, sizeof(*response));
    if (request) {
        copy_text(response->command_id, sizeof(response->command_id),
                  request->command_id);
        copy_text(response->trace_id, sizeof(response->trace_id),
                  request->trace_id);
    }
    response->outcome = CMPP_OUTCOME_REJECTED;
    response->effect = CMPP_EFFECT_NO_EFFECT;
    response->state_version = state_version;
    response_error(response, code, message);
}

static bool result_is_valid(const cmpp_result_t *result)
{
    if (result->outcome > CMPP_OUTCOME_FAILED ||
        result->effect > CMPP_EFFECT_UNKNOWN) {
        return false;
    }
    if (result->outcome == CMPP_OUTCOME_REJECTED &&
        result->effect != CMPP_EFFECT_NO_EFFECT) {
        return false;
    }
    if (result->outcome == CMPP_OUTCOME_COMPLETED &&
        result->effect == CMPP_EFFECT_UNKNOWN) {
        return false;
    }
    return true;
}

cmpp_status_t cmpp_runtime_create(const cmpp_platform_ops_t *platform,
                                  const cmpp_runtime_config_t *config,
                                  cmpp_runtime_t **output)
{
    if (!platform || !config || !output || !platform->allocate ||
        !platform->deallocate || !platform->monotonic_ms ||
        !platform->lock_create || !platform->lock_destroy ||
        !platform->lock_acquire || !platform->lock_release ||
        !valid_identifier(config->device_id) ||
        !valid_identifier(config->profile_id) ||
        config->max_capabilities == 0U || config->max_ttl_ms == 0U) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    *output = NULL;
    cmpp_runtime_t *runtime = platform->allocate(platform->context,
                                                  sizeof(*runtime));
    if (!runtime) {
        return CMPP_ERR_NO_MEMORY;
    }
    memset(runtime, 0, sizeof(*runtime));
    runtime->platform = *platform;
    runtime->config = *config;
    copy_text(runtime->device_id, sizeof(runtime->device_id), config->device_id);
    copy_text(runtime->profile_id, sizeof(runtime->profile_id), config->profile_id);
    runtime->config.device_id = runtime->device_id;
    runtime->config.profile_id = runtime->profile_id;
    runtime->capabilities = rt_alloc(
        runtime, config->max_capabilities * sizeof(*runtime->capabilities));
    if (config->idempotency_cache_size > 0U) {
        runtime->cache = rt_alloc(
            runtime, config->idempotency_cache_size * sizeof(*runtime->cache));
    }
    if (!runtime->capabilities ||
        (config->idempotency_cache_size > 0U && !runtime->cache)) {
        cmpp_runtime_destroy(runtime);
        return CMPP_ERR_NO_MEMORY;
    }
    memset(runtime->capabilities, 0,
           config->max_capabilities * sizeof(*runtime->capabilities));
    if (runtime->cache) {
        memset(runtime->cache, 0,
               config->idempotency_cache_size * sizeof(*runtime->cache));
    }
    runtime->metadata_lock = platform->lock_create(platform->context);
    runtime->execution_lock = platform->lock_create(platform->context);
    if (!runtime->metadata_lock || !runtime->execution_lock) {
        cmpp_runtime_destroy(runtime);
        return CMPP_ERR_NO_MEMORY;
    }
    if (config->persist_state_version && platform->storage_read) {
        size_t size = sizeof(runtime->state_version);
        if (platform->storage_read(platform->context, STATE_VERSION_KEY,
                                   &runtime->state_version, &size) != CMPP_OK ||
            size != sizeof(runtime->state_version)) {
            runtime->state_version = 0U;
        }
    }
    *output = runtime;
    return CMPP_OK;
}

void cmpp_runtime_destroy(cmpp_runtime_t *runtime)
{
    if (!runtime) {
        return;
    }
    if (runtime->cache) {
        for (size_t index = 0;
             index < runtime->config.idempotency_cache_size; ++index) {
            if (runtime->cache[index].occupied) {
                cmpp_response_deinit(runtime, &runtime->cache[index].response);
            }
        }
    }
    if (runtime->metadata_lock) {
        runtime->platform.lock_destroy(runtime->platform.context,
                                       runtime->metadata_lock);
    }
    if (runtime->execution_lock) {
        runtime->platform.lock_destroy(runtime->platform.context,
                                       runtime->execution_lock);
    }
    rt_free(runtime, runtime->cache);
    rt_free(runtime, runtime->capabilities);
    runtime->platform.deallocate(runtime->platform.context, runtime);
}

cmpp_status_t cmpp_runtime_register_capability(
    cmpp_runtime_t *runtime, const cmpp_capability_descriptor_t *descriptor,
    const cmpp_capability_ops_t *ops, void *module_context)
{
    if (!runtime || !descriptor || !ops ||
        !valid_identifier(descriptor->capability_id) ||
        !bounded_text(descriptor->version, CMPP_ID_MAX, false) ||
        !descriptor->command_types || descriptor->command_type_count == 0U ||
        descriptor->allowed_safety_mask == 0U || !ops->execute) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < descriptor->command_type_count; ++index) {
        if (!valid_identifier(descriptor->command_types[index])) {
            return CMPP_ERR_INVALID_ARGUMENT;
        }
    }
    if (!runtime->platform.lock_acquire(runtime->platform.context,
                                        runtime->metadata_lock,
                                        CMPP_WAIT_FOREVER)) {
        return CMPP_ERR_TIMEOUT;
    }
    cmpp_status_t status = CMPP_OK;
    if (runtime->capability_count >= runtime->config.max_capabilities) {
        status = CMPP_ERR_NO_MEMORY;
    } else if (find_capability(runtime, descriptor->capability_id)) {
        status = CMPP_ERR_CONFLICT;
    } else {
        capability_record_t *record =
            &runtime->capabilities[runtime->capability_count++];
        record->descriptor = descriptor;
        record->ops = *ops;
        record->context = module_context;
    }
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->metadata_lock);
    return status;
}

cmpp_status_t cmpp_runtime_unregister_capability(cmpp_runtime_t *runtime,
                                                 const char *capability_id)
{
    if (!runtime || !valid_identifier(capability_id)) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (!runtime->platform.lock_acquire(runtime->platform.context,
                                        runtime->execution_lock,
                                        CMPP_WAIT_FOREVER)) {
        return CMPP_ERR_TIMEOUT;
    }
    runtime->platform.lock_acquire(runtime->platform.context,
                                   runtime->metadata_lock,
                                   CMPP_WAIT_FOREVER);
    cmpp_status_t status = CMPP_ERR_NOT_FOUND;
    for (size_t index = 0; index < runtime->capability_count; ++index) {
        if (strcmp(runtime->capabilities[index].descriptor->capability_id,
                   capability_id) == 0) {
            for (size_t next = index + 1U;
                 next < runtime->capability_count; ++next) {
                runtime->capabilities[next - 1U] =
                    runtime->capabilities[next];
            }
            --runtime->capability_count;
            memset(&runtime->capabilities[runtime->capability_count], 0,
                   sizeof(runtime->capabilities[0]));
            status = CMPP_OK;
            break;
        }
    }
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->metadata_lock);
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->execution_lock);
    return status;
}

cmpp_status_t cmpp_runtime_dispatch(cmpp_runtime_t *runtime,
                                    const cmpp_request_t *request,
                                    cmpp_response_t *response)
{
    if (!runtime || !request || !response) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    memset(response, 0, sizeof(*response));
    const uint64_t received_at = runtime->platform.monotonic_ms(
        runtime->platform.context);
    capability_record_t *record = NULL;
    const char *validation_error = NULL;
    if (!runtime->platform.lock_acquire(runtime->platform.context,
                                        runtime->metadata_lock,
                                        CMPP_WAIT_FOREVER)) {
        return CMPP_ERR_TIMEOUT;
    }
    cmpp_status_t status = validate_envelope(runtime, request, &record,
                                             &validation_error);
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->metadata_lock);
    if (status == CMPP_OK && record->ops.validate) {
        status = record->ops.validate(record->context, request->arguments);
        if (status != CMPP_OK) {
            validation_error = "arguments_rejected";
        }
    }
    if (status != CMPP_OK) {
        rejection(response, request, state_version_snapshot(runtime),
                  validation_error ? validation_error : "invalid_request",
                  cmpp_status_string(status));
        return CMPP_OK;
    }
    const uint64_t deadline = deadline_for(runtime, request, received_at);
    uint32_t remaining = 0U;
    if (!deadline_remaining(runtime, deadline, &remaining)) {
        rejection(response, request, state_version_snapshot(runtime),
                  "deadline_exceeded", "command expired before execution");
        return CMPP_OK;
    }
    const uint64_t hash = request_hash(request);
    if (!runtime->platform.lock_acquire(runtime->platform.context,
                                        runtime->metadata_lock,
                                        CMPP_WAIT_FOREVER)) {
        return CMPP_ERR_TIMEOUT;
    }
    cache_lookup_t lookup = cache_lookup(runtime, request, hash, response);
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->metadata_lock);
    if (lookup == CACHE_MATCH) {
        return CMPP_OK;
    }
    if (lookup == CACHE_ERROR) {
        return CMPP_ERR_NO_MEMORY;
    }
    if (lookup == CACHE_COLLISION) {
        rejection(response, request, state_version_snapshot(runtime),
                  "command_id_collision",
                  "command id or idempotency key was reused");
        return CMPP_OK;
    }
    if (!runtime->platform.lock_acquire(runtime->platform.context,
                                        runtime->execution_lock, remaining)) {
        rejection(response, request, state_version_snapshot(runtime),
                  "deadline_exceeded_while_waiting",
                  "command expired while waiting for execution guard");
        return CMPP_OK;
    }
    if (!deadline_remaining(runtime, deadline, &remaining)) {
        rejection(response, request, state_version_snapshot(runtime),
                  "deadline_exceeded_while_waiting",
                  "command expired while waiting for execution guard");
        runtime->platform.lock_release(runtime->platform.context,
                                       runtime->execution_lock);
        return CMPP_OK;
    }
    runtime->platform.lock_acquire(runtime->platform.context,
                                   runtime->metadata_lock, CMPP_WAIT_FOREVER);
    lookup = cache_lookup(runtime, request, hash, response);
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->metadata_lock);
    if (lookup != CACHE_MISS) {
        if (lookup == CACHE_ERROR) {
            runtime->platform.lock_release(runtime->platform.context,
                                           runtime->execution_lock);
            return CMPP_ERR_NO_MEMORY;
        }
        if (lookup == CACHE_COLLISION) {
            rejection(response, request, state_version_snapshot(runtime),
                      "command_id_collision",
                      "command id or idempotency key was reused");
        }
        runtime->platform.lock_release(runtime->platform.context,
                                       runtime->execution_lock);
        return CMPP_OK;
    }
    if (record->ops.admit) {
        status = record->ops.admit(record->context, request);
        if (status != CMPP_OK) {
            rejection(response, request, state_version_snapshot(runtime),
                      "admission_rejected", cmpp_status_string(status));
            goto publish_and_cache;
        }
    }
    cmpp_result_t result = {
        .outcome = CMPP_OUTCOME_FAILED,
        .effect = CMPP_EFFECT_NO_EFFECT,
        .error_code = "execution_failed",
    };
    status = record->ops.execute(record->context, request, &result);
    if (status != CMPP_OK && result.outcome == CMPP_OUTCOME_COMPLETED) {
        result.outcome = CMPP_OUTCOME_FAILED;
    }
    if (!result_is_valid(&result)) {
        result.outcome = CMPP_OUTCOME_FAILED;
        result.effect = CMPP_EFFECT_NO_EFFECT;
        result.data = NULL;
        result.error_code = "invalid_module_result";
        result.error_message = "capability returned an invalid outcome/effect";
    }
    copy_text(response->command_id, sizeof(response->command_id),
              request->command_id);
    copy_text(response->trace_id, sizeof(response->trace_id), request->trace_id);
    response->outcome = result.outcome;
    response->effect = result.effect;
    if (result.data) {
        status = clone_value(runtime, result.data, &response->data);
        if (status != CMPP_OK) {
            response->outcome = CMPP_OUTCOME_FAILED;
            response->effect = result.effect == CMPP_EFFECT_COMMITTED
                                   ? CMPP_EFFECT_COMMITTED
                                   : CMPP_EFFECT_NO_EFFECT;
            response_error(response, "result_allocation_failed",
                           cmpp_status_string(status));
        } else {
            response->has_data = true;
        }
    }
    if (response->error_code[0] == '\0' &&
        response->outcome != CMPP_OUTCOME_COMPLETED) {
        response_error(response,
                       result.error_code ? result.error_code :
                                           cmpp_status_string(status),
                       result.error_message ? result.error_message :
                                              cmpp_status_string(status));
    }
    if (response->effect == CMPP_EFFECT_COMMITTED) {
        runtime->platform.lock_acquire(runtime->platform.context,
                                       runtime->metadata_lock,
                                       CMPP_WAIT_FOREVER);
        ++runtime->state_version;
        if (runtime->config.persist_state_version &&
            runtime->platform.storage_write) {
            cmpp_status_t storage_status = runtime->platform.storage_write(
                runtime->platform.context, STATE_VERSION_KEY,
                &runtime->state_version, sizeof(runtime->state_version));
            if (storage_status != CMPP_OK && runtime->platform.log) {
                runtime->platform.log(runtime->platform.context, CMPP_LOG_WARN,
                                      "could not persist state version");
            }
        }
        response->state_version = runtime->state_version;
        runtime->platform.lock_release(runtime->platform.context,
                                       runtime->metadata_lock);
    } else {
        response->state_version = state_version_snapshot(runtime);
    }

publish_and_cache:
    ;
    cmpp_event_callback_t event_callback = NULL;
    void *event_context = NULL;
    runtime->platform.lock_acquire(runtime->platform.context,
                                   runtime->metadata_lock,
                                   CMPP_WAIT_FOREVER);
    event_callback = runtime->config.event_callback;
    event_context = runtime->config.event_context;
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->metadata_lock);
    if (event_callback) {
        event_callback(event_context, response);
    }
    runtime->platform.lock_acquire(runtime->platform.context,
                                   runtime->metadata_lock, CMPP_WAIT_FOREVER);
    (void)cache_store(runtime, request, hash, response);
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->metadata_lock);
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->execution_lock);
    return CMPP_OK;
}

cmpp_status_t cmpp_runtime_get_info(cmpp_runtime_t *runtime,
                                    cmpp_runtime_info_t *info)
{
    if (!runtime || !info) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (!runtime->platform.lock_acquire(runtime->platform.context,
                                        runtime->metadata_lock,
                                        CMPP_WAIT_FOREVER)) {
        return CMPP_ERR_TIMEOUT;
    }
    info->device_id = runtime->device_id;
    info->profile_id = runtime->profile_id;
    info->state_version = runtime->state_version;
    info->capability_count = runtime->capability_count;
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->metadata_lock);
    return CMPP_OK;
}

cmpp_status_t cmpp_runtime_set_event_callback(cmpp_runtime_t *runtime,
                                              cmpp_event_callback_t callback,
                                              void *context)
{
    if (!runtime) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (!runtime->platform.lock_acquire(runtime->platform.context,
                                        runtime->metadata_lock,
                                        CMPP_WAIT_FOREVER)) {
        return CMPP_ERR_TIMEOUT;
    }
    runtime->config.event_callback = callback;
    runtime->config.event_context = context;
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->metadata_lock);
    return CMPP_OK;
}

cmpp_status_t cmpp_runtime_get_capability(
    cmpp_runtime_t *runtime, size_t index,
    const cmpp_capability_descriptor_t **descriptor)
{
    if (!runtime || !descriptor) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (!runtime->platform.lock_acquire(runtime->platform.context,
                                        runtime->metadata_lock,
                                        CMPP_WAIT_FOREVER)) {
        return CMPP_ERR_TIMEOUT;
    }
    cmpp_status_t status = CMPP_OK;
    if (index >= runtime->capability_count) {
        status = CMPP_ERR_INVALID_ARGUMENT;
    } else {
        *descriptor = runtime->capabilities[index].descriptor;
    }
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->metadata_lock);
    return status;
}

cmpp_status_t cmpp_runtime_observe(cmpp_runtime_t *runtime,
                                   const char *capability_id,
                                   cmpp_value_t *state)
{
    if (!runtime || !capability_id || !state) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    runtime->platform.lock_acquire(runtime->platform.context,
                                   runtime->metadata_lock,
                                   CMPP_WAIT_FOREVER);
    capability_record_t *record = find_capability(runtime, capability_id);
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->metadata_lock);
    if (!record) {
        return CMPP_ERR_NOT_FOUND;
    }
    if (!record->ops.observe) {
        return CMPP_ERR_NOT_SUPPORTED;
    }
    if (!runtime->platform.lock_acquire(runtime->platform.context,
                                        runtime->execution_lock,
                                        CMPP_WAIT_FOREVER)) {
        return CMPP_ERR_TIMEOUT;
    }
    cmpp_value_t borrowed_state;
    memset(&borrowed_state, 0, sizeof(borrowed_state));
    cmpp_status_t status = record->ops.observe(record->context,
                                               &borrowed_state);
    if (status == CMPP_OK) {
        status = clone_value(runtime, &borrowed_state, state);
    }
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->execution_lock);
    return status;
}

cmpp_status_t cmpp_runtime_cancel(cmpp_runtime_t *runtime,
                                  const char *capability_id,
                                  const char *command_id)
{
    if (!runtime || !capability_id || !valid_identifier(command_id)) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    runtime->platform.lock_acquire(runtime->platform.context,
                                   runtime->metadata_lock,
                                   CMPP_WAIT_FOREVER);
    capability_record_t *record = find_capability(runtime, capability_id);
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->metadata_lock);
    return !record ? CMPP_ERR_NOT_FOUND
                   : !record->ops.cancel ? CMPP_ERR_NOT_SUPPORTED
                                         : record->ops.cancel(record->context,
                                                              command_id);
}

cmpp_status_t cmpp_runtime_safe_stop(cmpp_runtime_t *runtime)
{
    if (!runtime) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (!runtime->platform.lock_acquire(runtime->platform.context,
                                        runtime->execution_lock,
                                        CMPP_WAIT_FOREVER)) {
        return CMPP_ERR_TIMEOUT;
    }
    runtime->platform.lock_acquire(runtime->platform.context,
                                   runtime->metadata_lock,
                                   CMPP_WAIT_FOREVER);
    const size_t capability_count = runtime->capability_count;
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->metadata_lock);
    cmpp_status_t overall = CMPP_OK;
    for (size_t index = 0; index < capability_count; ++index) {
        if (runtime->capabilities[index].ops.safe_stop) {
            cmpp_status_t status = runtime->capabilities[index].ops.safe_stop(
                runtime->capabilities[index].context);
            if (status != CMPP_OK) {
                overall = status;
            }
        }
    }
    runtime->platform.lock_release(runtime->platform.context,
                                   runtime->execution_lock);
    return overall;
}

const cmpp_value_t *cmpp_value_object_get(const cmpp_value_t *object,
                                          const char *key)
{
    if (!object || object->type != CMPP_VALUE_OBJECT || !key) {
        return NULL;
    }
    for (size_t index = 0; index < object->as.object.count; ++index) {
        if (strcmp(object->as.object.entries[index].key, key) == 0) {
            return object->as.object.entries[index].value;
        }
    }
    return NULL;
}

const char *cmpp_value_get_string(const cmpp_value_t *value)
{
    return value && value->type == CMPP_VALUE_STRING ? value->as.string : NULL;
}

bool cmpp_value_get_bool(const cmpp_value_t *value, bool *result)
{
    if (!value || value->type != CMPP_VALUE_BOOL || !result) {
        return false;
    }
    *result = value->as.boolean;
    return true;
}

bool cmpp_value_get_int(const cmpp_value_t *value, int64_t *result)
{
    if (!value || value->type != CMPP_VALUE_INT || !result) {
        return false;
    }
    *result = value->as.integer;
    return true;
}

bool cmpp_value_get_double(const cmpp_value_t *value, double *result)
{
    if (!value || !result) {
        return false;
    }
    if (value->type == CMPP_VALUE_DOUBLE) {
        *result = value->as.number;
        return true;
    }
    if (value->type == CMPP_VALUE_INT) {
        *result = (double)value->as.integer;
        return true;
    }
    return false;
}

const char *cmpp_status_string(cmpp_status_t status)
{
    static const char *const names[] = {
        "ok", "invalid_argument", "invalid_state", "no_memory",
        "not_found", "not_supported", "timeout", "conflict", "io_error",
        "internal_error",
    };
    return (unsigned)status < sizeof(names) / sizeof(names[0])
               ? names[status]
               : "unknown_status";
}

const char *cmpp_outcome_string(cmpp_outcome_t outcome)
{
    static const char *const names[] = {"rejected", "completed", "failed"};
    return (unsigned)outcome < sizeof(names) / sizeof(names[0])
               ? names[outcome]
               : "invalid";
}

const char *cmpp_effect_string(cmpp_effect_t effect)
{
    static const char *const names[] = {"no_effect", "committed", "unknown"};
    return (unsigned)effect < sizeof(names) / sizeof(names[0])
               ? names[effect]
               : "invalid";
}

const char *cmpp_safety_class_string(cmpp_safety_class_t safety_class)
{
    static const char *const names[] = {
        "benign", "reversible", "irreversible", "critical"};
    return (unsigned)safety_class < sizeof(names) / sizeof(names[0])
               ? names[safety_class]
               : "invalid";
}

bool cmpp_safety_class_parse(const char *text,
                             cmpp_safety_class_t *safety_class)
{
    if (!text || !safety_class) {
        return false;
    }
    for (unsigned index = 0; index < CMPP_SAFETY_CLASS_COUNT; ++index) {
        if (strcmp(text, cmpp_safety_class_string((cmpp_safety_class_t)index)) ==
            0) {
            *safety_class = (cmpp_safety_class_t)index;
            return true;
        }
    }
    return false;
}
