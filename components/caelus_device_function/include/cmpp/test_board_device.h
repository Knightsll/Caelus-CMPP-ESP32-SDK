/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "cmpp/cmpp.h"
#include "cmpp/cmpp_backend_pwm.h"
#include "cmpp/cmpp_backend_relay.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CMPP_TEST_BOARD_PWM_COUNT 6U
#define CMPP_TEST_BOARD_RELAY_COUNT 4U

#define CMPP_TEST_BOARD_PWM_CAPABILITY "test_board.pwm"
#define CMPP_TEST_BOARD_RELAY_CAPABILITY "test_board.relay"
#define CMPP_TEST_BOARD_PWM_SET "test_board.pwm.set"
#define CMPP_TEST_BOARD_RELAY_SET "test_board.relay.set"
#define CMPP_TEST_BOARD_RELAY_ALL_OFF "test_board.relay.all_off"

typedef struct cmpp_test_board_device cmpp_test_board_device_t;

typedef struct {
    cmpp_pwm_backend_t *pwm[CMPP_TEST_BOARD_PWM_COUNT];
    cmpp_relay_backend_t *relay[CMPP_TEST_BOARD_RELAY_COUNT];
} cmpp_test_board_device_config_t;

cmpp_status_t cmpp_test_board_device_register(
    cmpp_runtime_t *runtime,
    const cmpp_test_board_device_config_t *config,
    cmpp_test_board_device_t **device);
void cmpp_test_board_device_destroy(cmpp_test_board_device_t *device);

#ifdef __cplusplus
}
#endif

