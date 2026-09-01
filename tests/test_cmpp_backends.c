/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/cmpp_backend_pwm.h"
#include "cmpp/cmpp_backend_relay.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint32_t duty = 0U;
    assert(cmpp_pwm_map_duty(10U, false, 0U, &duty) == CMPP_OK);
    assert(duty == 0U);
    assert(cmpp_pwm_map_duty(10U, false, CMPP_PWM_NORMALIZED_MAX, &duty) ==
           CMPP_OK);
    assert(duty == 1023U);
    assert(cmpp_pwm_map_duty(10U, true, 0U, &duty) == CMPP_OK);
    assert(duty == 1023U);
    assert(cmpp_pwm_map_duty(10U, true, CMPP_PWM_NORMALIZED_MAX, &duty) ==
           CMPP_OK);
    assert(duty == 0U);

    const cmpp_pwm_config_t pwm = {
        .output_pin = 4,
        .frequency_hz = 20000U,
        .resolution_bits = 10U,
        .minimum_output = 1000U,
        .maximum_output = 8000U,
        .startup_output = 0U,
        .safe_stop_output = 0U,
        .ramp_step = 100U,
        .ramp_interval_ms = 10U,
    };
    assert(cmpp_pwm_config_is_consistent(&pwm));

    assert(cmpp_relay_physical_level(true, true) == 1);
    assert(cmpp_relay_physical_level(true, false) == 0);
    assert(cmpp_relay_physical_level(false, true) == 0);
    assert(cmpp_relay_physical_level(false, false) == 1);

    puts("cmpp backend contract tests passed");
    return 0;
}
