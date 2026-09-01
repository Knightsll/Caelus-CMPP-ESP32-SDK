/* SPDX-License-Identifier: Apache-2.0
 * Generic backend example. Pin and polarity belong to board assembly.
 */
#include "cmpp/cmpp_backend_relay.h"

cmpp_status_t example_relay_start(int board_relay_pin,
                                  cmpp_relay_backend_t **backend)
{
    const cmpp_relay_config_t config = {
        .output_pin = board_relay_pin,
        .active_high = false,
        .boot_on = false,
        .safe_stop_on = false,
        .minimum_switch_interval_ms = 500,
    };
    return cmpp_relay_create(&config, backend);
}

cmpp_status_t example_relay_apply(cmpp_relay_backend_t *backend, bool on)
{
    return cmpp_relay_set(backend, on);
}

