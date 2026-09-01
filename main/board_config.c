/* SPDX-License-Identifier: Apache-2.0 */
#include "board_config.h"

#include <stdlib.h>

#include "cmpp/cmpp_backend_pwm.h"
#include "cmpp/cmpp_backend_relay.h"
#include "cmpp/test_board_device.h"

#define BOARD_PWM_FREQUENCY_HZ 5000U
#define BOARD_PWM_RESOLUTION_BITS 10U
#define BOARD_RELAY_MINIMUM_SWITCH_MS 100U

static const int PWM_PINS[CMPP_TEST_BOARD_PWM_COUNT] = {7, 8, 9, 10, 20, 21};
static const int RELAY_PINS[CMPP_TEST_BOARD_RELAY_COUNT] = {3, 2, 1, 0};

struct board_runtime {
    cmpp_runtime_t *runtime;
    cmpp_pwm_backend_t *pwm[CMPP_TEST_BOARD_PWM_COUNT];
    cmpp_relay_backend_t *relay[CMPP_TEST_BOARD_RELAY_COUNT];
    cmpp_test_board_device_t *device;
};

static esp_err_t esp_error_from_cmpp(cmpp_status_t status)
{
    switch (status) {
    case CMPP_OK:
        return ESP_OK;
    case CMPP_ERR_NO_MEMORY:
        return ESP_ERR_NO_MEM;
    case CMPP_ERR_INVALID_ARGUMENT:
        return ESP_ERR_INVALID_ARG;
    case CMPP_ERR_NOT_SUPPORTED:
        return ESP_ERR_NOT_SUPPORTED;
    case CMPP_ERR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    default:
        return ESP_FAIL;
    }
}

esp_err_t board_runtime_create(cmpp_runtime_t *runtime,
                               board_runtime_t **output)
{
    if (!runtime || !output) {
        return ESP_ERR_INVALID_ARG;
    }
    *output = NULL;
    board_runtime_t *board = calloc(1U, sizeof(*board));
    if (!board) {
        return ESP_ERR_NO_MEM;
    }
    board->runtime = runtime;
    cmpp_status_t status = CMPP_OK;
    for (size_t index = 0; index < CMPP_TEST_BOARD_PWM_COUNT; ++index) {
        const cmpp_pwm_config_t config = {
            .output_pin = PWM_PINS[index],
            .channel = index,
            .timer = 0U,
            .frequency_hz = BOARD_PWM_FREQUENCY_HZ,
            .resolution_bits = BOARD_PWM_RESOLUTION_BITS,
            .inverted = false,
            .minimum_output = 0U,
            .maximum_output = CMPP_PWM_NORMALIZED_MAX,
            .startup_output = 0U,
            .safe_stop_output = 0U,
            .ramp_step = 100U,
            .ramp_interval_ms = 10U,
        };
        status = cmpp_pwm_create(&config, &board->pwm[index]);
        if (status != CMPP_OK) {
            board_runtime_destroy(board);
            return esp_error_from_cmpp(status);
        }
    }
    for (size_t index = 0; index < CMPP_TEST_BOARD_RELAY_COUNT; ++index) {
        const cmpp_relay_config_t config = {
            .output_pin = RELAY_PINS[index],
            .active_high = true,
            .boot_on = false,
            .safe_stop_on = false,
            .minimum_switch_interval_ms = BOARD_RELAY_MINIMUM_SWITCH_MS,
        };
        status = cmpp_relay_create(&config, &board->relay[index]);
        if (status != CMPP_OK) {
            board_runtime_destroy(board);
            return esp_error_from_cmpp(status);
        }
    }
    cmpp_test_board_device_config_t device_config = {0};
    for (size_t index = 0; index < CMPP_TEST_BOARD_PWM_COUNT; ++index) {
        device_config.pwm[index] = board->pwm[index];
    }
    for (size_t index = 0; index < CMPP_TEST_BOARD_RELAY_COUNT; ++index) {
        device_config.relay[index] = board->relay[index];
    }
    status = cmpp_test_board_device_register(runtime, &device_config,
                                              &board->device);
    if (status != CMPP_OK) {
        board_runtime_destroy(board);
        return esp_error_from_cmpp(status);
    }
    *output = board;
    return ESP_OK;
}

esp_err_t board_runtime_safe_stop(board_runtime_t *board)
{
    if (!board) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_error_from_cmpp(cmpp_runtime_safe_stop(board->runtime));
}

void board_runtime_destroy(board_runtime_t *board)
{
    if (!board) {
        return;
    }
    (void)board_runtime_safe_stop(board);
    if (board->device) {
        cmpp_test_board_device_destroy(board->device);
    }
    for (size_t index = 0; index < CMPP_TEST_BOARD_PWM_COUNT; ++index) {
        cmpp_pwm_destroy(board->pwm[index]);
    }
    for (size_t index = 0; index < CMPP_TEST_BOARD_RELAY_COUNT; ++index) {
        cmpp_relay_destroy(board->relay[index]);
    }
    free(board);
}
