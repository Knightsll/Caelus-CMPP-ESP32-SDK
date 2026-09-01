/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "cmpp/cmpp.h"
#include "device_identity.h"
#include "esp_err.h"

esp_err_t cmpp_device_server_start(const device_identity_t *identity,
                                   cmpp_runtime_t *runtime);

