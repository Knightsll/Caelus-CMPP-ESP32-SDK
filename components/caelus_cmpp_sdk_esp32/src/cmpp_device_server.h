/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "cmpp/cmpp.h"
#include "device_identity.h"
#include "esp_err.h"

typedef struct {
    const char *server_name;
    const char *server_description;
    const char *device_type;
} cmpp_device_server_config_t;

esp_err_t cmpp_device_server_start(const device_identity_t *identity,
                                   cmpp_runtime_t *runtime,
                                   const cmpp_device_server_config_t *config);
