/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/cmpp.h"
#include "cmpp/cmpp_codec_json.h"
#include "cmpp/cmpp_mcp_binding.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

typedef struct {
    bool stored;
    uint64_t stored_version;
    atomic_size_t active_allocations;
} test_platform_t;

typedef struct {
    pthread_mutex_t mutex;
} test_lock_t;

typedef struct {
    pthread_mutex_t mutex;
    unsigned executions;
    unsigned active;
    unsigned maximum_active;
    unsigned sleep_ms;
    cmpp_status_t execute_status;
    cmpp_outcome_t outcome;
    cmpp_effect_t effect;
    bool return_data;
    cmpp_value_t result_value;
} test_module_t;

typedef struct {
    unsigned count;
    cmpp_outcome_t last_outcome;
    cmpp_effect_t last_effect;
    uint64_t last_version;
} test_events_t;

static void sleep_ms(unsigned milliseconds)
{
    struct timespec duration = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    (void)nanosleep(&duration, NULL);
}

static void *test_allocate(void *context, size_t size)
{
    test_platform_t *platform = context;
    void *memory = calloc(1U, size);
    if (memory) {
        (void)atomic_fetch_add(&platform->active_allocations, 1U);
    }
    return memory;
}

static void test_deallocate(void *context, void *memory)
{
    test_platform_t *platform = context;
    if (memory) {
        (void)atomic_fetch_sub(&platform->active_allocations, 1U);
    }
    free(memory);
}

static uint64_t test_now(void *context)
{
    (void)context;
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static cmpp_lock_handle_t test_lock_create(void *context)
{
    (void)context;
    test_lock_t *lock = calloc(1U, sizeof(*lock));
    if (!lock || pthread_mutex_init(&lock->mutex, NULL) != 0) {
        free(lock);
        return NULL;
    }
    return lock;
}

static void test_lock_destroy(void *context, cmpp_lock_handle_t handle)
{
    (void)context;
    test_lock_t *lock = handle;
    assert(pthread_mutex_destroy(&lock->mutex) == 0);
    free(lock);
}

static bool test_lock_acquire(void *context, cmpp_lock_handle_t handle,
                              uint32_t timeout_ms)
{
    (void)context;
    test_lock_t *lock = handle;
    if (timeout_ms == CMPP_WAIT_FOREVER) {
        return pthread_mutex_lock(&lock->mutex) == 0;
    }
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return pthread_mutex_timedlock(&lock->mutex, &deadline) == 0;
}

static void test_lock_release(void *context, cmpp_lock_handle_t handle)
{
    (void)context;
    test_lock_t *lock = handle;
    assert(pthread_mutex_unlock(&lock->mutex) == 0);
}

static cmpp_status_t test_storage_read(void *context, const char *key,
                                       void *data, size_t *size)
{
    test_platform_t *platform = context;
    (void)key;
    if (!platform->stored || *size < sizeof(platform->stored_version)) {
        return CMPP_ERR_NOT_FOUND;
    }
    memcpy(data, &platform->stored_version, sizeof(platform->stored_version));
    *size = sizeof(platform->stored_version);
    return CMPP_OK;
}

static cmpp_status_t test_storage_write(void *context, const char *key,
                                        const void *data, size_t size)
{
    test_platform_t *platform = context;
    (void)key;
    if (size != sizeof(platform->stored_version)) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    memcpy(&platform->stored_version, data, size);
    platform->stored = true;
    return CMPP_OK;
}

static cmpp_platform_ops_t platform_ops(test_platform_t *platform)
{
    return (cmpp_platform_ops_t){
        .context = platform,
        .allocate = test_allocate,
        .deallocate = test_deallocate,
        .monotonic_ms = test_now,
        .lock_create = test_lock_create,
        .lock_destroy = test_lock_destroy,
        .lock_acquire = test_lock_acquire,
        .lock_release = test_lock_release,
        .storage_read = test_storage_read,
        .storage_write = test_storage_write,
    };
}

static cmpp_status_t validate_arguments(void *context,
                                        const cmpp_value_t *arguments)
{
    (void)context;
    int64_t value = 0;
    return cmpp_value_get_int(cmpp_value_object_get(arguments, "value"),
                              &value) && value >= 0 && value <= 100
               ? CMPP_OK
               : CMPP_ERR_INVALID_ARGUMENT;
}

static cmpp_status_t execute(void *context, const cmpp_request_t *request,
                             cmpp_result_t *result)
{
    (void)request;
    test_module_t *module = context;
    pthread_mutex_lock(&module->mutex);
    ++module->executions;
    ++module->active;
    if (module->active > module->maximum_active) {
        module->maximum_active = module->active;
    }
    pthread_mutex_unlock(&module->mutex);
    sleep_ms(module->sleep_ms);
    pthread_mutex_lock(&module->mutex);
    --module->active;
    pthread_mutex_unlock(&module->mutex);
    result->outcome = module->outcome;
    result->effect = module->effect;
    result->error_code = module->outcome == CMPP_OUTCOME_COMPLETED
                             ? NULL
                             : "module_failure";
    result->data = module->return_data ? &module->result_value : NULL;
    return module->execute_status;
}

static void receive_event(void *context, const cmpp_response_t *response)
{
    test_events_t *events = context;
    ++events->count;
    events->last_outcome = response->outcome;
    events->last_effect = response->effect;
    events->last_version = response->state_version;
}

static const char *const COMMAND_TYPES[] = {"test.set"};
static const cmpp_capability_descriptor_t DESCRIPTOR = {
    .capability_id = "test.output",
    .display_name = "Test output",
    .version = "1.0.0",
    .description = "Host contract test capability",
    .arguments_schema_json =
        "{\"type\":\"object\",\"required\":[\"value\"]}",
    .command_types = COMMAND_TYPES,
    .command_type_count = 1U,
    .allowed_safety_mask = CMPP_SAFETY_ALL_MASK,
    .requires_ack = true,
    .supports_dry_run = true,
};

static cmpp_runtime_t *make_runtime(test_platform_t *platform,
                                    test_module_t *module,
                                    test_events_t *events)
{
    cmpp_platform_ops_t ops = platform_ops(platform);
    cmpp_runtime_config_t config = {
        .device_id = "device.test-1",
        .profile_id = "profile.test",
        .max_capabilities = 4U,
        .idempotency_cache_size = 8U,
        .max_ttl_ms = 1000U,
        .persist_state_version = true,
        .event_callback = receive_event,
        .event_context = events,
    };
    cmpp_runtime_t *runtime = NULL;
    assert(cmpp_runtime_create(&ops, &config, &runtime) == CMPP_OK);
    cmpp_capability_ops_t capability_ops = {
        .validate = validate_arguments,
        .execute = execute,
    };
    assert(cmpp_runtime_register_capability(runtime, &DESCRIPTOR,
                                            &capability_ops, module) == CMPP_OK);
    return runtime;
}

typedef struct {
    cmpp_value_t argument_value;
    cmpp_object_entry_t argument_entry;
    cmpp_value_t arguments;
    cmpp_request_t request;
} request_fixture_t;

static request_fixture_t request_fixture(const char *command_id,
                                         const char *idempotency_key,
                                         int64_t value, uint32_t ttl_ms)
{
    request_fixture_t fixture = {0};
    fixture.argument_value = (cmpp_value_t){
        .type = CMPP_VALUE_INT,
        .as.integer = value,
    };
    fixture.argument_entry = (cmpp_object_entry_t){
        .key = "value",
        .value = &fixture.argument_value,
    };
    fixture.arguments = (cmpp_value_t){
        .type = CMPP_VALUE_OBJECT,
        .as.object = {
            .entries = &fixture.argument_entry,
            .count = 1U,
        },
    };
    fixture.request = (cmpp_request_t){
        .schema = CMPP_COMMAND_SCHEMA,
        .protocol_version = CMPP_PROTOCOL_VERSION,
        .command_id = command_id,
        .trace_id = "trace.test",
        .device_id = "device.test-1",
        .capability_id = "test.output",
        .command_type = "test.set",
        .requested_by = "host-tests",
        .reason = "contract test",
        .idempotency_key = idempotency_key,
        .safety_class = CMPP_SAFETY_REVERSIBLE,
        .arguments = &fixture.arguments,
        .ttl_ms = ttl_ms,
        .requires_ack = true,
    };
    return fixture;
}

static void repair_fixture_pointers(request_fixture_t *fixture)
{
    fixture->argument_entry.value = &fixture->argument_value;
    fixture->arguments.as.object.entries = &fixture->argument_entry;
    fixture->request.arguments = &fixture->arguments;
}

static void test_validation_and_idempotency(cmpp_runtime_t *runtime,
                                            test_module_t *module,
                                            test_events_t *events)
{
    request_fixture_t fixture = request_fixture("cmd.1", "idem.1", 42, 500U);
    repair_fixture_pointers(&fixture);
    module->return_data = true;
    module->result_value = (cmpp_value_t){
        .type = CMPP_VALUE_STRING,
        .as.string = "applied",
    };
    cmpp_response_t response;
    assert(cmpp_runtime_dispatch(runtime, &fixture.request, &response) == CMPP_OK);
    assert(response.outcome == CMPP_OUTCOME_COMPLETED);
    assert(response.effect == CMPP_EFFECT_COMMITTED);
    assert(response.state_version == 1U);
    assert(response.has_data);
    assert(strcmp(cmpp_value_get_string(&response.data), "applied") == 0);
    assert(events->count == 1U && events->last_version == 1U);
    cmpp_response_deinit(runtime, &response);

    assert(cmpp_runtime_dispatch(runtime, &fixture.request, &response) == CMPP_OK);
    assert(response.from_idempotency_cache);
    assert(response.state_version == 1U);
    assert(response.has_data);
    assert(strcmp(cmpp_value_get_string(&response.data), "applied") == 0);
    assert(module->executions == 1U);
    assert(events->count == 1U);
    cmpp_response_deinit(runtime, &response);
    module->return_data = false;

    fixture.argument_value.as.integer = 43;
    assert(cmpp_runtime_dispatch(runtime, &fixture.request, &response) == CMPP_OK);
    assert(response.outcome == CMPP_OUTCOME_REJECTED);
    assert(strcmp(response.error_code, "command_id_collision") == 0);

    request_fixture_t wrong = request_fixture("cmd.2", "idem.2", 20, 500U);
    repair_fixture_pointers(&wrong);
    wrong.request.device_id = "device.wrong";
    assert(cmpp_runtime_dispatch(runtime, &wrong.request, &response) == CMPP_OK);
    assert(strcmp(response.error_code, "wrong_device_target") == 0);

    wrong.request.device_id = "device.test-1";
    wrong.argument_value.as.integer = 101;
    assert(cmpp_runtime_dispatch(runtime, &wrong.request, &response) == CMPP_OK);
    assert(strcmp(response.error_code, "arguments_rejected") == 0);

    wrong.argument_value.as.integer = 20;
    wrong.request.capability_id = "missing.capability";
    assert(cmpp_runtime_dispatch(runtime, &wrong.request, &response) == CMPP_OK);
    assert(strcmp(response.error_code, "unsupported_capability_or_command") ==
           0);

    wrong.request.capability_id = "test.output";
    wrong.request.absolute_deadline_ms = 1U;
    assert(cmpp_runtime_dispatch(runtime, &wrong.request, &response) == CMPP_OK);
    assert(strcmp(response.error_code, "deadline_exceeded") == 0);

    wrong.request.absolute_deadline_ms = 0U;
    wrong.request.requires_ack = false;
    assert(cmpp_runtime_dispatch(runtime, &wrong.request, &response) == CMPP_OK);
    assert(strcmp(response.error_code, "safety_metadata_rejected") == 0);

    cmpp_value_t values[] = {
        {.type = CMPP_VALUE_INT, .as.integer = 21},
        {.type = CMPP_VALUE_BOOL, .as.boolean = true},
    };
    cmpp_object_entry_t first_order[] = {
        {.key = "value", .value = &values[0]},
        {.key = "enabled", .value = &values[1]},
    };
    cmpp_object_entry_t second_order[] = {
        {.key = "enabled", .value = &values[1]},
        {.key = "value", .value = &values[0]},
    };
    request_fixture_t ordered = request_fixture("cmd.order", "idem.order", 21,
                                                500U);
    repair_fixture_pointers(&ordered);
    ordered.arguments.as.object.entries = first_order;
    ordered.arguments.as.object.count = 2U;
    const unsigned executions_before = module->executions;
    assert(cmpp_runtime_dispatch(runtime, &ordered.request, &response) == CMPP_OK);
    cmpp_response_deinit(runtime, &response);
    ordered.arguments.as.object.entries = second_order;
    assert(cmpp_runtime_dispatch(runtime, &ordered.request, &response) == CMPP_OK);
    assert(response.from_idempotency_cache);
    assert(module->executions == executions_before + 1U);
    cmpp_response_deinit(runtime, &response);
}

typedef struct {
    cmpp_runtime_t *runtime;
    request_fixture_t fixture;
    cmpp_response_t response;
} dispatch_thread_t;

static void *dispatch_thread(void *context)
{
    dispatch_thread_t *dispatch = context;
    repair_fixture_pointers(&dispatch->fixture);
    assert(cmpp_runtime_dispatch(dispatch->runtime, &dispatch->fixture.request,
                                 &dispatch->response) == CMPP_OK);
    return NULL;
}

static void test_concurrency_and_deadline(cmpp_runtime_t *runtime,
                                          test_module_t *module)
{
    module->sleep_ms = 100U;
    dispatch_thread_t first = {
        .runtime = runtime,
        .fixture = request_fixture("cmd.3", "idem.3", 30, 500U),
    };
    dispatch_thread_t second = {
        .runtime = runtime,
        .fixture = request_fixture("cmd.4", "idem.4", 40, 20U),
    };
    pthread_t first_thread;
    pthread_t second_thread;
    assert(pthread_create(&first_thread, NULL, dispatch_thread, &first) == 0);
    sleep_ms(10U);
    assert(pthread_create(&second_thread, NULL, dispatch_thread, &second) == 0);
    assert(pthread_join(first_thread, NULL) == 0);
    assert(pthread_join(second_thread, NULL) == 0);
    assert(first.response.outcome == CMPP_OUTCOME_COMPLETED);
    assert(second.response.outcome == CMPP_OUTCOME_REJECTED);
    assert(strcmp(second.response.error_code,
                  "deadline_exceeded_while_waiting") == 0);
    assert(module->maximum_active == 1U);
    cmpp_response_deinit(runtime, &first.response);
    cmpp_response_deinit(runtime, &second.response);
    module->sleep_ms = 0U;
}

static void test_failure_effects(cmpp_runtime_t *runtime,
                                 test_module_t *module)
{
    cmpp_response_t response;
    module->execute_status = CMPP_ERR_IO;
    module->outcome = CMPP_OUTCOME_FAILED;
    module->effect = CMPP_EFFECT_NO_EFFECT;
    request_fixture_t before = request_fixture("cmd.5", "idem.5", 50, 500U);
    repair_fixture_pointers(&before);
    assert(cmpp_runtime_dispatch(runtime, &before.request, &response) == CMPP_OK);
    const uint64_t version = response.state_version;
    assert(response.outcome == CMPP_OUTCOME_FAILED);
    assert(response.effect == CMPP_EFFECT_NO_EFFECT);
    cmpp_response_deinit(runtime, &response);

    module->effect = CMPP_EFFECT_COMMITTED;
    request_fixture_t after = request_fixture("cmd.6", "idem.6", 60, 500U);
    repair_fixture_pointers(&after);
    assert(cmpp_runtime_dispatch(runtime, &after.request, &response) == CMPP_OK);
    assert(response.outcome == CMPP_OUTCOME_FAILED);
    assert(response.effect == CMPP_EFFECT_COMMITTED);
    assert(response.state_version == version + 1U);
    cmpp_response_deinit(runtime, &response);

    module->execute_status = CMPP_OK;
    module->outcome = CMPP_OUTCOME_COMPLETED;
    module->effect = CMPP_EFFECT_UNKNOWN;
    request_fixture_t invalid = request_fixture("cmd.7", "idem.7", 70, 500U);
    repair_fixture_pointers(&invalid);
    assert(cmpp_runtime_dispatch(runtime, &invalid.request, &response) == CMPP_OK);
    assert(response.outcome == CMPP_OUTCOME_FAILED);
    assert(response.effect == CMPP_EFFECT_NO_EFFECT);
    assert(strcmp(response.error_code, "invalid_module_result") == 0);
    cmpp_response_deinit(runtime, &response);
}

static void test_mcp_and_in_process_mapping(cmpp_runtime_t *runtime,
                                            test_module_t *module)
{
    module->execute_status = CMPP_OK;
    module->outcome = CMPP_OUTCOME_COMPLETED;
    module->effect = CMPP_EFFECT_COMMITTED;
    const char *mcp_command =
        "{\"schema\":\"cmpp.command.v1\",\"protocol_version\":\"1.0\","
        "\"command_id\":\"cmd.mcp\",\"trace_id\":\"trace.mcp\","
        "\"target\":{\"device_id\":\"device.test-1\","
        "\"capability_id\":\"test.output\"},"
        "\"command_type\":\"test.set\",\"requested_by\":\"host-tests\","
        "\"reason\":\"MCP binding test\",\"safety_class\":\"reversible\","
        "\"arguments\":{\"value\":80},\"ttl_ms\":500,"
        "\"idempotency_key\":\"idem.mcp\",\"requires_ack\":true}";
    char *mcp_result = NULL;
    assert(cmpp_mcp_dispatch_json(runtime, mcp_command, &mcp_result) == CMPP_OK);
    assert(strstr(mcp_result, "\"status\":\"completed\"") != NULL);
    assert(strstr(mcp_result, "\"effect\":\"committed\"") != NULL);
    cmpp_mcp_free_json(mcp_result);

    const char *in_process_command =
        "{\"schema\":\"cmpp.command.v1\",\"protocol_version\":\"1.0\","
        "\"command_id\":\"cmd.local\",\"trace_id\":\"trace.local\","
        "\"target\":{\"device_id\":\"device.test-1\","
        "\"capability_id\":\"test.output\"},"
        "\"command_type\":\"test.set\",\"requested_by\":\"host-tests\","
        "\"reason\":\"in-process adapter test\","
        "\"safety_class\":\"reversible\",\"arguments\":{\"value\":80},"
        "\"ttl_ms\":500,\"idempotency_key\":\"idem.local\","
        "\"requires_ack\":true}";
    cmpp_json_document_t *document = NULL;
    cmpp_request_t request;
    assert(cmpp_json_decode_request(in_process_command, &document, &request) ==
           CMPP_OK);
    cmpp_response_t response;
    assert(cmpp_runtime_dispatch(runtime, &request, &response) == CMPP_OK);
    assert(response.outcome == CMPP_OUTCOME_COMPLETED);
    assert(response.effect == CMPP_EFFECT_COMMITTED);
    cmpp_response_deinit(runtime, &response);
    cmpp_json_document_destroy(document);

    assert(strcmp(cmpp_mcp_protocol_version(), "2025-11-25") == 0);
    assert(strcmp(cmpp_mcp_discovery_service(), "_cmpp._tcp") == 0);
    assert(cmpp_mcp_dispatch_json(runtime, "not-json", &mcp_result) ==
           CMPP_ERR_INVALID_ARGUMENT);
}

static void test_heap_stability(cmpp_runtime_t *runtime,
                                test_platform_t *platform,
                                test_module_t *module)
{
    module->execute_status = CMPP_OK;
    module->outcome = CMPP_OUTCOME_COMPLETED;
    module->effect = CMPP_EFFECT_NO_EFFECT;
    for (unsigned index = 0; index < 8U; ++index) {
        char command_id[32];
        char idempotency_key[32];
        (void)snprintf(command_id, sizeof(command_id), "cmd.warm.%u", index);
        (void)snprintf(idempotency_key, sizeof(idempotency_key), "idem.warm.%u",
                       index);
        request_fixture_t fixture = request_fixture(command_id, idempotency_key,
                                                    index, 500U);
        repair_fixture_pointers(&fixture);
        cmpp_response_t response;
        assert(cmpp_runtime_dispatch(runtime, &fixture.request, &response) ==
               CMPP_OK);
        cmpp_response_deinit(runtime, &response);
    }
    const size_t baseline = atomic_load(&platform->active_allocations);
    for (unsigned index = 0; index < 200U; ++index) {
        char command_id[32];
        char idempotency_key[32];
        (void)snprintf(command_id, sizeof(command_id), "cmd.heap.%u", index);
        (void)snprintf(idempotency_key, sizeof(idempotency_key), "idem.heap.%u",
                       index);
        request_fixture_t fixture = request_fixture(command_id, idempotency_key,
                                                    index % 100U, 500U);
        repair_fixture_pointers(&fixture);
        cmpp_response_t response;
        assert(cmpp_runtime_dispatch(runtime, &fixture.request, &response) ==
               CMPP_OK);
        cmpp_response_deinit(runtime, &response);
    }
    assert(atomic_load(&platform->active_allocations) == baseline);
}

int main(void)
{
    test_platform_t platform = {0};
    test_module_t module = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .execute_status = CMPP_OK,
        .outcome = CMPP_OUTCOME_COMPLETED,
        .effect = CMPP_EFFECT_COMMITTED,
        .result_value = {.type = CMPP_VALUE_NULL},
    };
    test_events_t events = {0};
    cmpp_runtime_t *runtime = make_runtime(&platform, &module, &events);

    test_validation_and_idempotency(runtime, &module, &events);
    test_concurrency_and_deadline(runtime, &module);
    test_failure_effects(runtime, &module);
    test_mcp_and_in_process_mapping(runtime, &module);
    test_heap_stability(runtime, &platform, &module);

    cmpp_runtime_info_t info;
    assert(cmpp_runtime_get_info(runtime, &info) == CMPP_OK);
    const uint64_t persisted_version = info.state_version;
    assert(platform.stored && platform.stored_version == persisted_version);
    cmpp_runtime_destroy(runtime);

    test_events_t reboot_events = {0};
    runtime = make_runtime(&platform, &module, &reboot_events);
    assert(cmpp_runtime_get_info(runtime, &info) == CMPP_OK);
    assert(info.state_version == persisted_version);
    cmpp_runtime_destroy(runtime);
    assert(pthread_mutex_destroy(&module.mutex) == 0);
    assert(atomic_load(&platform.active_allocations) == 0U);

    puts("cmpp_core contract tests passed");
    return 0;
}
