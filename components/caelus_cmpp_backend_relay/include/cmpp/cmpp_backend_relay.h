/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "cmpp/cmpp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cmpp_relay_backend cmpp_relay_backend_t;

typedef struct {
    int output_pin;
    bool active_high;
    bool boot_on;
    bool safe_stop_on;
    uint32_t minimum_switch_interval_ms;
} cmpp_relay_config_t;

int cmpp_relay_physical_level(bool active_high, bool on);

cmpp_status_t cmpp_relay_create(const cmpp_relay_config_t *config,
                                cmpp_relay_backend_t **backend);
void cmpp_relay_destroy(cmpp_relay_backend_t *backend);
cmpp_status_t cmpp_relay_set(cmpp_relay_backend_t *backend, bool on);
cmpp_status_t cmpp_relay_safe_stop(cmpp_relay_backend_t *backend);
cmpp_status_t cmpp_relay_get_state(cmpp_relay_backend_t *backend, bool *on);

#ifdef __cplusplus
}
#endif
