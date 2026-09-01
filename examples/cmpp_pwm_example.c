/* SPDX-License-Identifier: Apache-2.0
 * Generic backend example. Pin selection belongs to board assembly.
 */
#include "cmpp/cmpp_backend_pwm.h"

cmpp_status_t example_pwm_start(int board_pwm_pin,
                                cmpp_pwm_backend_t **backend)
{
    const cmpp_pwm_config_t config = {
        .output_pin = board_pwm_pin,
        .channel = 0,
        .timer = 0,
        .frequency_hz = 20000,
        .resolution_bits = 10,
        .inverted = false,
        .minimum_output = 1000,
        .maximum_output = 8000,
        .startup_output = 0,
        .safe_stop_output = 0,
        .ramp_step = 100,
        .ramp_interval_ms = 10,
    };
    return cmpp_pwm_create(&config, backend);
}

cmpp_status_t example_pwm_apply(cmpp_pwm_backend_t *backend,
                                uint16_t normalized_output)
{
    return cmpp_pwm_set(backend, normalized_output, 250);
}

