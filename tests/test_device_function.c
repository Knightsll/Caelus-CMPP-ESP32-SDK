/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/test_board_device.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct cmpp_pwm_backend {
    uint16_t output;
    unsigned set_count;
    unsigned safe_stop_count;
};

struct cmpp_relay_backend {
    bool on;
    unsigned set_count;
    unsigned safe_stop_count;
};

cmpp_status_t cmpp_pwm_set(cmpp_pwm_backend_t *backend,
                           uint16_t normalized_output,
                           uint32_t transition_ms)
{
    (void)transition_ms;
    if (!backend || normalized_output > CMPP_PWM_NORMALIZED_MAX) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    backend->output = normalized_output;
    ++backend->set_count;
    return CMPP_OK;
}

cmpp_status_t cmpp_pwm_safe_stop(cmpp_pwm_backend_t *backend)
{
    if (!backend) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    backend->output = 0U;
    ++backend->safe_stop_count;
    return CMPP_OK;
}

cmpp_status_t cmpp_pwm_get_output(cmpp_pwm_backend_t *backend,
                                  uint16_t *normalized_output)
{
    if (!backend || !normalized_output) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    *normalized_output = backend->output;
    return CMPP_OK;
}

cmpp_status_t cmpp_relay_set(cmpp_relay_backend_t *backend, bool on)
{
    if (!backend) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    backend->on = on;
    ++backend->set_count;
    return CMPP_OK;
}

cmpp_status_t cmpp_relay_safe_stop(cmpp_relay_backend_t *backend)
{
    if (!backend) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    backend->on = false;
    ++backend->safe_stop_count;
    return CMPP_OK;
}

cmpp_status_t cmpp_relay_get_state(cmpp_relay_backend_t *backend, bool *on)
{
    if (!backend || !on) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    *on = backend->on;
    return CMPP_OK;
}

typedef struct {
    pthread_mutex_t mutex;
} test_lock_t;

static void *test_allocate(void *context, size_t size)
{
    (void)context;
    return calloc(1U, size);
}

static void test_deallocate(void *context, void *memory)
{
    (void)context;
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
    (void)timeout_ms;
    test_lock_t *lock = handle;
    return pthread_mutex_lock(&lock->mutex) == 0;
}

static void test_lock_release(void *context, cmpp_lock_handle_t handle)
{
    (void)context;
    test_lock_t *lock = handle;
    assert(pthread_mutex_unlock(&lock->mutex) == 0);
}

typedef struct {
    cmpp_value_t values[3];
    cmpp_object_entry_t entries[3];
    cmpp_value_t arguments;
    cmpp_request_t request;
} command_fixture_t;

static void init_request(command_fixture_t *fixture, const char *command_id,
                         const char *capability_id, const char *command_type,
                         size_t argument_count)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->arguments = (cmpp_value_t){
        .type = CMPP_VALUE_OBJECT,
        .as.object = {
            .entries = fixture->entries,
            .count = argument_count,
        },
    };
    fixture->request = (cmpp_request_t){
        .schema = CMPP_COMMAND_SCHEMA,
        .protocol_version = CMPP_PROTOCOL_VERSION,
        .command_id = command_id,
        .trace_id = "trace.device-function",
        .device_id = "device.test-board",
        .capability_id = capability_id,
        .command_type = command_type,
        .requested_by = "device-function-tests",
        .reason = "contract test",
        .idempotency_key = command_id,
        .safety_class = CMPP_SAFETY_REVERSIBLE,
        .arguments = &fixture->arguments,
        .ttl_ms = 1000U,
        .requires_ack = true,
    };
}

static void set_int(command_fixture_t *fixture, size_t index,
                    const char *key, int64_t value)
{
    fixture->values[index] = (cmpp_value_t){
        .type = CMPP_VALUE_INT,
        .as.integer = value,
    };
    fixture->entries[index] = (cmpp_object_entry_t){
        .key = key,
        .value = &fixture->values[index],
    };
}

static void set_bool(command_fixture_t *fixture, size_t index,
                     const char *key, bool value)
{
    fixture->values[index] = (cmpp_value_t){
        .type = CMPP_VALUE_BOOL,
        .as.boolean = value,
    };
    fixture->entries[index] = (cmpp_object_entry_t){
        .key = key,
        .value = &fixture->values[index],
    };
}

static void expect_completed(cmpp_runtime_t *runtime,
                             const cmpp_request_t *request,
                             cmpp_effect_t effect)
{
    cmpp_response_t response;
    assert(cmpp_runtime_dispatch(runtime, request, &response) == CMPP_OK);
    assert(response.outcome == CMPP_OUTCOME_COMPLETED);
    assert(response.effect == effect);
    cmpp_response_deinit(runtime, &response);
}

int main(void)
{
    const cmpp_platform_ops_t platform = {
        .allocate = test_allocate,
        .deallocate = test_deallocate,
        .monotonic_ms = test_now,
        .lock_create = test_lock_create,
        .lock_destroy = test_lock_destroy,
        .lock_acquire = test_lock_acquire,
        .lock_release = test_lock_release,
    };
    const cmpp_runtime_config_t runtime_config = {
        .device_id = "device.test-board",
        .profile_id = "caelus.test-board.v1",
        .max_capabilities = 2U,
        .idempotency_cache_size = 4U,
        .max_ttl_ms = 2000U,
    };
    cmpp_runtime_t *runtime = NULL;
    assert(cmpp_runtime_create(&platform, &runtime_config, &runtime) == CMPP_OK);

    struct cmpp_pwm_backend pwm[CMPP_TEST_BOARD_PWM_COUNT] = {0};
    struct cmpp_relay_backend relay[CMPP_TEST_BOARD_RELAY_COUNT] = {0};
    cmpp_test_board_device_config_t device_config = {0};
    for (size_t index = 0; index < CMPP_TEST_BOARD_PWM_COUNT; ++index) {
        device_config.pwm[index] = &pwm[index];
    }
    for (size_t index = 0; index < CMPP_TEST_BOARD_RELAY_COUNT; ++index) {
        device_config.relay[index] = &relay[index];
    }
    cmpp_test_board_device_t *device = NULL;
    assert(cmpp_test_board_device_register(runtime, &device_config, &device) ==
           CMPP_OK);

    command_fixture_t pwm_set;
    init_request(&pwm_set, "cmd.pwm.1", CMPP_TEST_BOARD_PWM_CAPABILITY,
                 CMPP_TEST_BOARD_PWM_SET, 3U);
    set_int(&pwm_set, 0U, "channel", 2);
    set_int(&pwm_set, 1U, "output", 7500);
    set_int(&pwm_set, 2U, "transition_ms", 250);
    expect_completed(runtime, &pwm_set.request, CMPP_EFFECT_COMMITTED);
    assert(pwm[1].output == 7500U && pwm[1].set_count == 1U);

    command_fixture_t pwm_dry_run;
    init_request(&pwm_dry_run, "cmd.pwm.dry", CMPP_TEST_BOARD_PWM_CAPABILITY,
                 CMPP_TEST_BOARD_PWM_SET, 2U);
    set_int(&pwm_dry_run, 0U, "channel", 2);
    set_int(&pwm_dry_run, 1U, "output", 1000);
    pwm_dry_run.request.dry_run = true;
    expect_completed(runtime, &pwm_dry_run.request, CMPP_EFFECT_NO_EFFECT);
    assert(pwm[1].output == 7500U && pwm[1].set_count == 1U);

    command_fixture_t relay_set;
    init_request(&relay_set, "cmd.relay.1", CMPP_TEST_BOARD_RELAY_CAPABILITY,
                 CMPP_TEST_BOARD_RELAY_SET, 2U);
    set_int(&relay_set, 0U, "channel", 3);
    set_bool(&relay_set, 1U, "on", true);
    expect_completed(runtime, &relay_set.request, CMPP_EFFECT_COMMITTED);
    assert(relay[2].on && relay[2].set_count == 1U);

    cmpp_value_t observed = {0};
    assert(cmpp_runtime_observe(runtime, CMPP_TEST_BOARD_PWM_CAPABILITY,
                                &observed) == CMPP_OK);
    const cmpp_value_t *channels = cmpp_value_object_get(&observed, "channels");
    assert(channels && channels->type == CMPP_VALUE_ARRAY);
    assert(channels->as.array.count == CMPP_TEST_BOARD_PWM_COUNT);
    int64_t output = 0;
    assert(cmpp_value_get_int(&channels->as.array.items[1], &output));
    assert(output == 7500);
    cmpp_value_deinit(runtime, &observed);

    command_fixture_t relay_all_off;
    init_request(&relay_all_off, "cmd.relay.off",
                 CMPP_TEST_BOARD_RELAY_CAPABILITY,
                 CMPP_TEST_BOARD_RELAY_ALL_OFF, 0U);
    expect_completed(runtime, &relay_all_off.request, CMPP_EFFECT_COMMITTED);
    for (size_t index = 0; index < CMPP_TEST_BOARD_RELAY_COUNT; ++index) {
        assert(!relay[index].on && relay[index].safe_stop_count == 1U);
    }

    command_fixture_t invalid_all_off;
    init_request(&invalid_all_off, "cmd.relay.invalid",
                 CMPP_TEST_BOARD_RELAY_CAPABILITY,
                 CMPP_TEST_BOARD_RELAY_ALL_OFF, 2U);
    set_int(&invalid_all_off, 0U, "channel", 1);
    set_bool(&invalid_all_off, 1U, "on", false);
    cmpp_response_t response;
    assert(cmpp_runtime_dispatch(runtime, &invalid_all_off.request, &response) ==
           CMPP_OK);
    assert(response.outcome == CMPP_OUTCOME_REJECTED);
    assert(strcmp(response.error_code, "admission_rejected") == 0);
    cmpp_response_deinit(runtime, &response);

    pwm[0].output = 5000U;
    relay[0].on = true;
    assert(cmpp_runtime_safe_stop(runtime) == CMPP_OK);
    for (size_t index = 0; index < CMPP_TEST_BOARD_PWM_COUNT; ++index) {
        assert(pwm[index].output == 0U && pwm[index].safe_stop_count == 1U);
    }
    for (size_t index = 0; index < CMPP_TEST_BOARD_RELAY_COUNT; ++index) {
        assert(!relay[index].on && relay[index].safe_stop_count == 2U);
    }

    cmpp_test_board_device_destroy(device);
    cmpp_runtime_info_t info;
    assert(cmpp_runtime_get_info(runtime, &info) == CMPP_OK);
    assert(info.capability_count == 0U);
    cmpp_runtime_destroy(runtime);

    puts("cmpp_test_board device function tests passed");
    return 0;
}
