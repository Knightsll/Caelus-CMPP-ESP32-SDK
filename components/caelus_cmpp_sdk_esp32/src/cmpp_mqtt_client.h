#pragma once

#include "cmpp/cmpp.h"
#include "device_identity.h"
#include "esp_err.h"

esp_err_t cmpp_mqtt_client_start(const device_identity_t *identity,
                                 cmpp_runtime_t *runtime);
