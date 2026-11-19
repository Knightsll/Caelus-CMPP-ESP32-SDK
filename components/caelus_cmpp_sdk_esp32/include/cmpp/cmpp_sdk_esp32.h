/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cmpp/cmpp.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CMPP_ESP32_SDK_VERSION "1.0.0"

typedef struct cmpp_esp32_sdk cmpp_esp32_sdk_t;

typedef struct {
    const char *profile_id;
    const char *server_name;
    const char *server_description;
    const char *device_type;
    size_t max_capabilities;
    size_t idempotency_cache_size;
    uint32_t max_ttl_ms;
    bool persist_state_version;
    cmpp_event_callback_t event_callback;
    void *event_context;
} cmpp_esp32_sdk_config_t;

esp_err_t cmpp_esp32_sdk_create(const cmpp_esp32_sdk_config_t *config,
                                cmpp_esp32_sdk_t **sdk);
cmpp_runtime_t *cmpp_esp32_sdk_runtime(cmpp_esp32_sdk_t *sdk);
const char *cmpp_esp32_sdk_device_uid(const cmpp_esp32_sdk_t *sdk);
esp_err_t cmpp_esp32_sdk_start(cmpp_esp32_sdk_t *sdk);

/* Destruction is supported only before start. A started SDK owns lifetime
 * services and is intended to live until the ESP32 reboots. */
esp_err_t cmpp_esp32_sdk_destroy(cmpp_esp32_sdk_t *sdk);

#ifdef __cplusplus
}
#endif
