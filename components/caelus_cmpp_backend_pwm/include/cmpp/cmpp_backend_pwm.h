/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "cmpp/cmpp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CMPP_PWM_NORMALIZED_MAX 10000U

typedef struct cmpp_pwm_backend cmpp_pwm_backend_t;

typedef struct {
    int output_pin;
    unsigned channel;
    unsigned timer;
    uint32_t frequency_hz;
    unsigned resolution_bits;
    bool inverted;
    uint16_t minimum_output;
    uint16_t maximum_output;
    uint16_t startup_output;
    uint16_t safe_stop_output;
    uint16_t ramp_step;
    uint32_t ramp_interval_ms;
} cmpp_pwm_config_t;

bool cmpp_pwm_config_is_consistent(const cmpp_pwm_config_t *config);
cmpp_status_t cmpp_pwm_map_duty(unsigned resolution_bits, bool inverted,
                                uint16_t normalized_output,
                                uint32_t *duty);

cmpp_status_t cmpp_pwm_create(const cmpp_pwm_config_t *config,
                              cmpp_pwm_backend_t **backend);
void cmpp_pwm_destroy(cmpp_pwm_backend_t *backend);
cmpp_status_t cmpp_pwm_set(cmpp_pwm_backend_t *backend,
                           uint16_t normalized_output,
                           uint32_t transition_ms);
cmpp_status_t cmpp_pwm_safe_stop(cmpp_pwm_backend_t *backend);
cmpp_status_t cmpp_pwm_get_output(cmpp_pwm_backend_t *backend,
                                  uint16_t *normalized_output);

#ifdef __cplusplus
}
#endif
