/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/cmpp_backend_pwm.h"

bool cmpp_pwm_config_is_consistent(const cmpp_pwm_config_t *config)
{
    return config && config->output_pin >= 0 && config->frequency_hz > 0U &&
           config->resolution_bits > 0U && config->resolution_bits <= 14U &&
           config->minimum_output <= config->maximum_output &&
           config->maximum_output <= CMPP_PWM_NORMALIZED_MAX &&
           config->startup_output <= CMPP_PWM_NORMALIZED_MAX &&
           config->safe_stop_output <= CMPP_PWM_NORMALIZED_MAX &&
           ((config->ramp_step == 0U) ==
            (config->ramp_interval_ms == 0U));
}

cmpp_status_t cmpp_pwm_map_duty(unsigned resolution_bits, bool inverted,
                                uint16_t normalized_output,
                                uint32_t *duty)
{
    if (!duty || resolution_bits == 0U || resolution_bits > 30U ||
        normalized_output > CMPP_PWM_NORMALIZED_MAX) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    const uint32_t maximum_duty = (UINT32_C(1) << resolution_bits) - 1U;
    uint32_t mapped = (uint32_t)(
        ((uint64_t)maximum_duty * normalized_output +
         CMPP_PWM_NORMALIZED_MAX / 2U) /
        CMPP_PWM_NORMALIZED_MAX);
    *duty = inverted ? maximum_duty - mapped : mapped;
    return CMPP_OK;
}

