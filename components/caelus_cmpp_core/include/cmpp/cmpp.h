/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Caelus contributors
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CMPP_COMMAND_SCHEMA "cmpp.command.v1"
#define CMPP_PROTOCOL_VERSION "1.0"
#define CMPP_WAIT_FOREVER UINT32_MAX
#define CMPP_ID_MAX 64U
#define CMPP_ERROR_CODE_MAX 64U
#define CMPP_ERROR_MESSAGE_MAX 128U

typedef enum {
    CMPP_OK = 0,
    CMPP_ERR_INVALID_ARGUMENT,
    CMPP_ERR_INVALID_STATE,
    CMPP_ERR_NO_MEMORY,
    CMPP_ERR_NOT_FOUND,
    CMPP_ERR_NOT_SUPPORTED,
    CMPP_ERR_TIMEOUT,
    CMPP_ERR_CONFLICT,
    CMPP_ERR_IO,
    CMPP_ERR_INTERNAL,
} cmpp_status_t;

typedef enum {
    CMPP_VALUE_NULL = 0,
    CMPP_VALUE_BOOL,
    CMPP_VALUE_INT,
    CMPP_VALUE_DOUBLE,
    CMPP_VALUE_STRING,
    CMPP_VALUE_ARRAY,
    CMPP_VALUE_OBJECT,
} cmpp_value_type_t;

typedef struct cmpp_value cmpp_value_t;

typedef struct {
    const cmpp_value_t *items;
    size_t count;
} cmpp_array_t;

typedef struct {
    const char *key;
    const cmpp_value_t *value;
} cmpp_object_entry_t;

typedef struct {
    const cmpp_object_entry_t *entries;
    size_t count;
} cmpp_object_t;

struct cmpp_value {
    cmpp_value_type_t type;
    union {
        bool boolean;
        int64_t integer;
        double number;
        const char *string;
        cmpp_array_t array;
        cmpp_object_t object;
    } as;
};

typedef enum {
    CMPP_SAFETY_BENIGN = 0,
    CMPP_SAFETY_REVERSIBLE,
    CMPP_SAFETY_IRREVERSIBLE,
    CMPP_SAFETY_CRITICAL,
    CMPP_SAFETY_CLASS_COUNT,
} cmpp_safety_class_t;

#define CMPP_SAFETY_MASK(value) (1UL << (unsigned)(value))
#define CMPP_SAFETY_ALL_MASK ((1UL << CMPP_SAFETY_CLASS_COUNT) - 1UL)

typedef enum {
    CMPP_OUTCOME_REJECTED = 0,
    CMPP_OUTCOME_COMPLETED,
    CMPP_OUTCOME_FAILED,
} cmpp_outcome_t;

typedef enum {
    CMPP_EFFECT_NO_EFFECT = 0,
    CMPP_EFFECT_COMMITTED,
    CMPP_EFFECT_UNKNOWN,
} cmpp_effect_t;

typedef struct {
    const char *schema;
    const char *protocol_version;
    const char *command_id;
    const char *trace_id;
    const char *device_id;
    const char *capability_id;
    const char *command_type;
    const char *requested_by;
    const char *reason;
    const char *idempotency_key;
    cmpp_safety_class_t safety_class;
    const cmpp_value_t *arguments;
    uint32_t ttl_ms;
    uint64_t absolute_deadline_ms;
    bool dry_run;
    bool requires_ack;
} cmpp_request_t;

typedef struct {
    cmpp_outcome_t outcome;
    cmpp_effect_t effect;
    const cmpp_value_t *data;
    const char *error_code;
    const char *error_message;
} cmpp_result_t;

typedef struct {
    char command_id[CMPP_ID_MAX + 1U];
    char trace_id[CMPP_ID_MAX + 1U];
    cmpp_outcome_t outcome;
    cmpp_effect_t effect;
    uint64_t state_version;
    bool from_idempotency_cache;
    bool has_data;
    cmpp_value_t data;
    char error_code[CMPP_ERROR_CODE_MAX + 1U];
    char error_message[CMPP_ERROR_MESSAGE_MAX + 1U];
} cmpp_response_t;

typedef struct {
    cmpp_status_t (*validate)(void *context, const cmpp_value_t *arguments);
    cmpp_status_t (*admit)(void *context, const cmpp_request_t *command);
    cmpp_status_t (*execute)(void *context, const cmpp_request_t *command,
                             cmpp_result_t *result);
    cmpp_status_t (*observe)(void *context, cmpp_value_t *state);
    cmpp_status_t (*cancel)(void *context, const char *command_id);
    cmpp_status_t (*safe_stop)(void *context);
} cmpp_capability_ops_t;

typedef struct {
    const char *capability_id;
    const char *display_name;
    const char *version;
    const char *description;
    const char *arguments_schema_json;
    const char *state_schema_json;
    const char *const *command_types;
    size_t command_type_count;
    uint32_t allowed_safety_mask;
    bool requires_ack;
    bool supports_dry_run;
} cmpp_capability_descriptor_t;

typedef void *cmpp_lock_handle_t;

typedef enum {
    CMPP_LOG_ERROR = 0,
    CMPP_LOG_WARN,
    CMPP_LOG_INFO,
    CMPP_LOG_DEBUG,
} cmpp_log_level_t;

typedef struct {
    void *context;
    void *(*allocate)(void *context, size_t size);
    void (*deallocate)(void *context, void *memory);
    uint64_t (*monotonic_ms)(void *context);
    cmpp_lock_handle_t (*lock_create)(void *context);
    void (*lock_destroy)(void *context, cmpp_lock_handle_t lock);
    bool (*lock_acquire)(void *context, cmpp_lock_handle_t lock,
                         uint32_t timeout_ms);
    void (*lock_release)(void *context, cmpp_lock_handle_t lock);
    cmpp_status_t (*storage_read)(void *context, const char *key,
                                  void *data, size_t *size);
    cmpp_status_t (*storage_write)(void *context, const char *key,
                                   const void *data, size_t size);
    cmpp_status_t (*random_fill)(void *context, void *data, size_t size);
    void (*log)(void *context, cmpp_log_level_t level, const char *message);
} cmpp_platform_ops_t;

typedef struct cmpp_runtime cmpp_runtime_t;

typedef void (*cmpp_event_callback_t)(void *context,
                                      const cmpp_response_t *response);

typedef struct {
    const char *device_id;
    const char *profile_id;
    size_t max_capabilities;
    size_t idempotency_cache_size;
    uint32_t max_ttl_ms;
    bool persist_state_version;
    cmpp_event_callback_t event_callback;
    void *event_context;
} cmpp_runtime_config_t;

typedef struct {
    const char *device_id;
    const char *profile_id;
    uint64_t state_version;
    size_t capability_count;
} cmpp_runtime_info_t;

cmpp_status_t cmpp_runtime_create(const cmpp_platform_ops_t *platform,
                                  const cmpp_runtime_config_t *config,
                                  cmpp_runtime_t **runtime);
void cmpp_runtime_destroy(cmpp_runtime_t *runtime);

cmpp_status_t cmpp_runtime_register_capability(
    cmpp_runtime_t *runtime, const cmpp_capability_descriptor_t *descriptor,
    const cmpp_capability_ops_t *ops, void *module_context);
cmpp_status_t cmpp_runtime_unregister_capability(cmpp_runtime_t *runtime,
                                                 const char *capability_id);

cmpp_status_t cmpp_runtime_dispatch(cmpp_runtime_t *runtime,
                                    const cmpp_request_t *request,
                                    cmpp_response_t *response);
void cmpp_response_deinit(cmpp_runtime_t *runtime, cmpp_response_t *response);
void cmpp_value_deinit(cmpp_runtime_t *runtime, cmpp_value_t *value);

cmpp_status_t cmpp_runtime_get_info(cmpp_runtime_t *runtime,
                                    cmpp_runtime_info_t *info);
cmpp_status_t cmpp_runtime_set_event_callback(cmpp_runtime_t *runtime,
                                              cmpp_event_callback_t callback,
                                              void *context);
cmpp_status_t cmpp_runtime_get_capability(
    cmpp_runtime_t *runtime, size_t index,
    const cmpp_capability_descriptor_t **descriptor);
cmpp_status_t cmpp_runtime_observe(cmpp_runtime_t *runtime,
                                   const char *capability_id,
                                   cmpp_value_t *state);
cmpp_status_t cmpp_runtime_cancel(cmpp_runtime_t *runtime,
                                  const char *capability_id,
                                  const char *command_id);
cmpp_status_t cmpp_runtime_safe_stop(cmpp_runtime_t *runtime);

const cmpp_value_t *cmpp_value_object_get(const cmpp_value_t *object,
                                          const char *key);
const char *cmpp_value_get_string(const cmpp_value_t *value);
bool cmpp_value_get_bool(const cmpp_value_t *value, bool *result);
bool cmpp_value_get_int(const cmpp_value_t *value, int64_t *result);
bool cmpp_value_get_double(const cmpp_value_t *value, double *result);

const char *cmpp_status_string(cmpp_status_t status);
const char *cmpp_outcome_string(cmpp_outcome_t outcome);
const char *cmpp_effect_string(cmpp_effect_t effect);
const char *cmpp_safety_class_string(cmpp_safety_class_t safety_class);
bool cmpp_safety_class_parse(const char *text,
                             cmpp_safety_class_t *safety_class);

#ifdef __cplusplus
}
#endif
