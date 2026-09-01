/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "cmpp/cmpp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cmpp_esp32_platform cmpp_esp32_platform_t;

typedef struct {
    const char *storage_namespace;
    void *allocator_context;
    void *(*allocate)(void *context, size_t size);
    void (*deallocate)(void *context, void *memory);
} cmpp_esp32_platform_config_t;

cmpp_status_t cmpp_esp32_platform_create(
    const cmpp_esp32_platform_config_t *config,
    cmpp_esp32_platform_t **platform);
void cmpp_esp32_platform_destroy(cmpp_esp32_platform_t *platform);
const cmpp_platform_ops_t *cmpp_esp32_platform_ops(
    cmpp_esp32_platform_t *platform);

#ifdef __cplusplus
}
#endif

