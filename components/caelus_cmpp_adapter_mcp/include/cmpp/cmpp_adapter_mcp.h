/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "cmpp/cmpp_mcp_binding.h"
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cmpp_mcp_adapter cmpp_mcp_adapter_t;

typedef struct {
    bool authenticated_transport;
} cmpp_mcp_adapter_config_t;

cmpp_status_t cmpp_mcp_adapter_create(
    esp_mcp_t *mcp, cmpp_runtime_t *runtime,
    const cmpp_mcp_adapter_config_t *config,
    cmpp_mcp_adapter_t **adapter);
void cmpp_mcp_adapter_destroy(cmpp_mcp_adapter_t *adapter);

/* Can be installed as the runtime event callback. */
void cmpp_mcp_adapter_publish_event(void *adapter,
                                    const cmpp_response_t *response);

#ifdef __cplusplus
}
#endif
