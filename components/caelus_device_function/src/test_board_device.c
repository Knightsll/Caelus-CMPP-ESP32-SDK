/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/test_board_device.h"

#include <stdlib.h>
#include <string.h>

#define PWM_TRANSITION_MAX_MS 60000U

struct cmpp_test_board_device {
    cmpp_runtime_t *runtime;
    cmpp_test_board_device_config_t hardware;
    cmpp_value_t observation_items[CMPP_TEST_BOARD_PWM_COUNT];
    cmpp_value_t observation_array;
    cmpp_object_entry_t observation_entry;
    cmpp_value_t result_values[2];
    cmpp_object_entry_t result_entries[2];
    cmpp_value_t result_object;
};

static const char *const PWM_COMMANDS[] = {CMPP_TEST_BOARD_PWM_SET};
static const char *const RELAY_COMMANDS[] = {
    CMPP_TEST_BOARD_RELAY_SET,
    CMPP_TEST_BOARD_RELAY_ALL_OFF,
};

static const cmpp_capability_descriptor_t PWM_DESCRIPTOR = {
    .capability_id = CMPP_TEST_BOARD_PWM_CAPABILITY,
    .display_name = "Test board indicator LEDs",
    .version = "1.0.0",
    .description = "Six active-high PWM indicator LEDs on the test board",
    .arguments_schema_json =
        "{\"oneOf\":[{\"type\":\"object\","
        "\"required\":[\"channel\",\"output\"],"
        "\"additionalProperties\":false,\"properties\":{"
        "\"channel\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":6},"
        "\"output\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10000},"
        "\"transition_ms\":{\"type\":\"integer\",\"minimum\":0,"
        "\"maximum\":60000}}}]}",
    .state_schema_json =
        "{\"type\":\"object\",\"required\":[\"channels\"],"
        "\"properties\":{\"channels\":{\"type\":\"array\","
        "\"minItems\":6,\"maxItems\":6,\"items\":{\"type\":\"integer\","
        "\"minimum\":0,\"maximum\":10000}}}}",
    .command_types = PWM_COMMANDS,
    .command_type_count = 1U,
    .allowed_safety_mask = CMPP_SAFETY_MASK(CMPP_SAFETY_BENIGN) |
                           CMPP_SAFETY_MASK(CMPP_SAFETY_REVERSIBLE),
    .requires_ack = false,
    .supports_dry_run = true,
};

static const cmpp_capability_descriptor_t RELAY_DESCRIPTOR = {
    .capability_id = CMPP_TEST_BOARD_RELAY_CAPABILITY,
    .display_name = "Test board relays",
    .version = "1.0.0",
    .description = "Four active-high NPN-driven relay channels",
    .arguments_schema_json =
        "{\"oneOf\":[{\"type\":\"object\","
        "\"required\":[\"channel\",\"on\"],\"additionalProperties\":false,"
        "\"properties\":{\"channel\":{\"type\":\"integer\","
        "\"minimum\":1,\"maximum\":4},\"on\":{\"type\":\"boolean\"}}},"
        "{\"type\":\"object\",\"maxProperties\":0}]}",
    .state_schema_json =
        "{\"type\":\"object\",\"required\":[\"channels\"],"
        "\"properties\":{\"channels\":{\"type\":\"array\","
        "\"minItems\":4,\"maxItems\":4,\"items\":{\"type\":\"boolean\"}}}}",
    .command_types = RELAY_COMMANDS,
    .command_type_count = 2U,
    .allowed_safety_mask = CMPP_SAFETY_MASK(CMPP_SAFETY_REVERSIBLE),
    .requires_ack = true,
    .supports_dry_run = true,
};

static bool exact_keys(const cmpp_value_t *arguments,
                       const char *const *keys, size_t count)
{
    if (!arguments || arguments->type != CMPP_VALUE_OBJECT ||
        arguments->as.object.count != count) {
        return false;
    }
    for (size_t index = 0; index < count; ++index) {
        if (!cmpp_value_object_get(arguments, keys[index])) {
            return false;
        }
    }
    return true;
}

static bool get_channel(const cmpp_value_t *arguments, size_t maximum,
                        size_t *channel)
{
    int64_t value = 0;
    if (!cmpp_value_get_int(cmpp_value_object_get(arguments, "channel"),
                            &value) ||
        value < 1 || value > (int64_t)maximum) {
        return false;
    }
    *channel = (size_t)value - 1U;
    return true;
}

static cmpp_status_t validate_pwm(void *context,
                                  const cmpp_value_t *arguments)
{
    (void)context;
    static const char *const required[] = {"channel", "output"};
    static const char *const with_transition[] = {
        "channel", "output", "transition_ms"};
    const bool keys_valid = exact_keys(arguments, required, 2U) ||
                            exact_keys(arguments, with_transition, 3U);
    size_t channel = 0U;
    int64_t output = 0;
    int64_t transition = 0;
    if (!keys_valid ||
        !get_channel(arguments, CMPP_TEST_BOARD_PWM_COUNT, &channel) ||
        !cmpp_value_get_int(cmpp_value_object_get(arguments, "output"),
                            &output) ||
        output < 0 || output > CMPP_PWM_NORMALIZED_MAX) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    const cmpp_value_t *transition_value =
        cmpp_value_object_get(arguments, "transition_ms");
    if (transition_value &&
        (!cmpp_value_get_int(transition_value, &transition) || transition < 0 ||
         transition > PWM_TRANSITION_MAX_MS)) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    (void)channel;
    return CMPP_OK;
}

static cmpp_status_t validate_relay(void *context,
                                    const cmpp_value_t *arguments)
{
    (void)context;
    if (!arguments || arguments->type != CMPP_VALUE_OBJECT) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (arguments->as.object.count == 0U) {
        return CMPP_OK;
    }
    static const char *const keys[] = {"channel", "on"};
    size_t channel = 0U;
    bool on = false;
    return exact_keys(arguments, keys, 2U) &&
                   get_channel(arguments, CMPP_TEST_BOARD_RELAY_COUNT,
                               &channel) &&
                   cmpp_value_get_bool(cmpp_value_object_get(arguments, "on"),
                                       &on)
               ? CMPP_OK
               : CMPP_ERR_INVALID_ARGUMENT;
}

static cmpp_status_t admit_relay(void *context,
                                 const cmpp_request_t *command)
{
    (void)context;
    if (strcmp(command->command_type, CMPP_TEST_BOARD_RELAY_ALL_OFF) == 0) {
        return command->arguments->as.object.count == 0U
                   ? CMPP_OK
                   : CMPP_ERR_INVALID_ARGUMENT;
    }
    static const char *const keys[] = {"channel", "on"};
    return exact_keys(command->arguments, keys, 2U)
               ? CMPP_OK
               : CMPP_ERR_INVALID_ARGUMENT;
}

static void prepare_result(cmpp_test_board_device_t *device,
                           size_t channel, const char *value_key,
                           cmpp_value_t value, cmpp_result_t *result)
{
    device->result_values[0] = (cmpp_value_t){
        .type = CMPP_VALUE_INT,
        .as.integer = (int64_t)channel + 1,
    };
    device->result_values[1] = value;
    device->result_entries[0] = (cmpp_object_entry_t){
        .key = "channel",
        .value = &device->result_values[0],
    };
    device->result_entries[1] = (cmpp_object_entry_t){
        .key = value_key,
        .value = &device->result_values[1],
    };
    device->result_object = (cmpp_value_t){
        .type = CMPP_VALUE_OBJECT,
        .as.object = {
            .entries = device->result_entries,
            .count = 2U,
        },
    };
    result->data = &device->result_object;
}

static cmpp_status_t execute_pwm(void *context,
                                 const cmpp_request_t *command,
                                 cmpp_result_t *result)
{
    cmpp_test_board_device_t *device = context;
    size_t channel = 0U;
    int64_t output = 0;
    int64_t transition = 0;
    (void)get_channel(command->arguments, CMPP_TEST_BOARD_PWM_COUNT, &channel);
    (void)cmpp_value_get_int(cmpp_value_object_get(command->arguments, "output"),
                             &output);
    const cmpp_value_t *transition_value =
        cmpp_value_object_get(command->arguments, "transition_ms");
    if (transition_value) {
        (void)cmpp_value_get_int(transition_value, &transition);
    }
    cmpp_status_t status = command->dry_run
                               ? CMPP_OK
                               : cmpp_pwm_set(device->hardware.pwm[channel],
                                              (uint16_t)output,
                                              (uint32_t)transition);
    result->outcome = status == CMPP_OK ? CMPP_OUTCOME_COMPLETED
                                        : CMPP_OUTCOME_FAILED;
    result->effect = command->dry_run
                         ? CMPP_EFFECT_NO_EFFECT
                         : status == CMPP_OK
                               ? CMPP_EFFECT_COMMITTED
                               : status == CMPP_ERR_IO ? CMPP_EFFECT_UNKNOWN
                                                       : CMPP_EFFECT_NO_EFFECT;
    result->error_code = status == CMPP_OK ? NULL : cmpp_status_string(status);
    prepare_result(device, channel, "output",
                   (cmpp_value_t){.type = CMPP_VALUE_INT,
                                  .as.integer = output},
                   result);
    return status;
}

static cmpp_status_t execute_relay(void *context,
                                   const cmpp_request_t *command,
                                   cmpp_result_t *result)
{
    cmpp_test_board_device_t *device = context;
    if (strcmp(command->command_type, CMPP_TEST_BOARD_RELAY_ALL_OFF) == 0) {
        cmpp_status_t status = CMPP_OK;
        bool committed = false;
        if (!command->dry_run) {
            for (size_t index = 0; index < CMPP_TEST_BOARD_RELAY_COUNT; ++index) {
                cmpp_status_t channel_status =
                    cmpp_relay_safe_stop(device->hardware.relay[index]);
                if (channel_status == CMPP_OK) {
                    committed = true;
                } else {
                    status = channel_status;
                }
            }
        }
        result->outcome = status == CMPP_OK ? CMPP_OUTCOME_COMPLETED
                                            : CMPP_OUTCOME_FAILED;
        result->effect = command->dry_run
                             ? CMPP_EFFECT_NO_EFFECT
                             : committed
                                   ? CMPP_EFFECT_COMMITTED
                                   : status == CMPP_OK
                                         ? CMPP_EFFECT_COMMITTED
                                         : CMPP_EFFECT_UNKNOWN;
        result->error_code = status == CMPP_OK ? NULL : cmpp_status_string(status);
        device->result_values[0] = (cmpp_value_t){
            .type = CMPP_VALUE_BOOL,
            .as.boolean = true,
        };
        device->result_entries[0] = (cmpp_object_entry_t){
            .key = "all_off",
            .value = &device->result_values[0],
        };
        device->result_object = (cmpp_value_t){
            .type = CMPP_VALUE_OBJECT,
            .as.object = {.entries = device->result_entries, .count = 1U},
        };
        result->data = &device->result_object;
        return status;
    }

    size_t channel = 0U;
    bool on = false;
    (void)get_channel(command->arguments, CMPP_TEST_BOARD_RELAY_COUNT, &channel);
    (void)cmpp_value_get_bool(cmpp_value_object_get(command->arguments, "on"),
                              &on);
    cmpp_status_t status = command->dry_run
                               ? CMPP_OK
                               : cmpp_relay_set(device->hardware.relay[channel],
                                                on);
    result->outcome = status == CMPP_OK ? CMPP_OUTCOME_COMPLETED
                                        : CMPP_OUTCOME_FAILED;
    result->effect = command->dry_run
                         ? CMPP_EFFECT_NO_EFFECT
                         : status == CMPP_OK
                               ? CMPP_EFFECT_COMMITTED
                               : status == CMPP_ERR_IO ? CMPP_EFFECT_UNKNOWN
                                                       : CMPP_EFFECT_NO_EFFECT;
    result->error_code = status == CMPP_OK ? NULL : cmpp_status_string(status);
    prepare_result(device, channel, "on",
                   (cmpp_value_t){.type = CMPP_VALUE_BOOL, .as.boolean = on},
                   result);
    return status;
}

static cmpp_status_t observe_pwm(void *context, cmpp_value_t *state)
{
    cmpp_test_board_device_t *device = context;
    for (size_t index = 0; index < CMPP_TEST_BOARD_PWM_COUNT; ++index) {
        uint16_t output = 0U;
        cmpp_status_t status = cmpp_pwm_get_output(device->hardware.pwm[index],
                                                   &output);
        if (status != CMPP_OK) {
            return status;
        }
        device->observation_items[index] = (cmpp_value_t){
            .type = CMPP_VALUE_INT,
            .as.integer = output,
        };
    }
    device->observation_array = (cmpp_value_t){
        .type = CMPP_VALUE_ARRAY,
        .as.array = {.items = device->observation_items,
                     .count = CMPP_TEST_BOARD_PWM_COUNT},
    };
    device->observation_entry = (cmpp_object_entry_t){
        .key = "channels",
        .value = &device->observation_array,
    };
    *state = (cmpp_value_t){
        .type = CMPP_VALUE_OBJECT,
        .as.object = {.entries = &device->observation_entry, .count = 1U},
    };
    return CMPP_OK;
}

static cmpp_status_t observe_relay(void *context, cmpp_value_t *state)
{
    cmpp_test_board_device_t *device = context;
    for (size_t index = 0; index < CMPP_TEST_BOARD_RELAY_COUNT; ++index) {
        bool on = false;
        cmpp_status_t status = cmpp_relay_get_state(device->hardware.relay[index],
                                                    &on);
        if (status != CMPP_OK) {
            return status;
        }
        device->observation_items[index] = (cmpp_value_t){
            .type = CMPP_VALUE_BOOL,
            .as.boolean = on,
        };
    }
    device->observation_array = (cmpp_value_t){
        .type = CMPP_VALUE_ARRAY,
        .as.array = {.items = device->observation_items,
                     .count = CMPP_TEST_BOARD_RELAY_COUNT},
    };
    device->observation_entry = (cmpp_object_entry_t){
        .key = "channels",
        .value = &device->observation_array,
    };
    *state = (cmpp_value_t){
        .type = CMPP_VALUE_OBJECT,
        .as.object = {.entries = &device->observation_entry, .count = 1U},
    };
    return CMPP_OK;
}

static cmpp_status_t safe_stop_pwm(void *context)
{
    cmpp_test_board_device_t *device = context;
    cmpp_status_t overall = CMPP_OK;
    for (size_t index = 0; index < CMPP_TEST_BOARD_PWM_COUNT; ++index) {
        cmpp_status_t status = cmpp_pwm_safe_stop(device->hardware.pwm[index]);
        if (status != CMPP_OK) {
            overall = status;
        }
    }
    return overall;
}

static cmpp_status_t safe_stop_relay(void *context)
{
    cmpp_test_board_device_t *device = context;
    cmpp_status_t overall = CMPP_OK;
    for (size_t index = 0; index < CMPP_TEST_BOARD_RELAY_COUNT; ++index) {
        cmpp_status_t status =
            cmpp_relay_safe_stop(device->hardware.relay[index]);
        if (status != CMPP_OK) {
            overall = status;
        }
    }
    return overall;
}

cmpp_status_t cmpp_test_board_device_register(
    cmpp_runtime_t *runtime,
    const cmpp_test_board_device_config_t *config,
    cmpp_test_board_device_t **output)
{
    if (!runtime || !config || !output) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < CMPP_TEST_BOARD_PWM_COUNT; ++index) {
        if (!config->pwm[index]) {
            return CMPP_ERR_INVALID_ARGUMENT;
        }
    }
    for (size_t index = 0; index < CMPP_TEST_BOARD_RELAY_COUNT; ++index) {
        if (!config->relay[index]) {
            return CMPP_ERR_INVALID_ARGUMENT;
        }
    }
    *output = NULL;
    cmpp_test_board_device_t *device = calloc(1U, sizeof(*device));
    if (!device) {
        return CMPP_ERR_NO_MEMORY;
    }
    device->runtime = runtime;
    device->hardware = *config;
    const cmpp_capability_ops_t pwm_ops = {
        .validate = validate_pwm,
        .execute = execute_pwm,
        .observe = observe_pwm,
        .safe_stop = safe_stop_pwm,
    };
    const cmpp_capability_ops_t relay_ops = {
        .validate = validate_relay,
        .admit = admit_relay,
        .execute = execute_relay,
        .observe = observe_relay,
        .safe_stop = safe_stop_relay,
    };
    cmpp_status_t status = cmpp_runtime_register_capability(
        runtime, &PWM_DESCRIPTOR, &pwm_ops, device);
    if (status == CMPP_OK) {
        status = cmpp_runtime_register_capability(
            runtime, &RELAY_DESCRIPTOR, &relay_ops, device);
    }
    if (status != CMPP_OK) {
        (void)cmpp_runtime_unregister_capability(
            runtime, CMPP_TEST_BOARD_PWM_CAPABILITY);
        free(device);
        return status;
    }
    *output = device;
    return CMPP_OK;
}

void cmpp_test_board_device_destroy(cmpp_test_board_device_t *device)
{
    if (!device) {
        return;
    }
    (void)cmpp_runtime_unregister_capability(
        device->runtime, CMPP_TEST_BOARD_PWM_CAPABILITY);
    (void)cmpp_runtime_unregister_capability(
        device->runtime, CMPP_TEST_BOARD_RELAY_CAPABILITY);
    free(device);
}
