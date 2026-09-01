/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/cmpp_platform_esp32.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define DEFAULT_NAMESPACE "cmpp"
#define NAMESPACE_MAX 15U

struct cmpp_esp32_platform {
    cmpp_platform_ops_t ops;
    char storage_namespace[NAMESPACE_MAX + 1U];
    void *allocator_context;
    void *(*allocator)(void *context, size_t size);
    void (*deallocator)(void *context, void *memory);
};

static void *default_allocate(void *context, size_t size)
{
    (void)context;
    return calloc(1U, size);
}

static void default_deallocate(void *context, void *memory)
{
    (void)context;
    free(memory);
}

static void *platform_allocate(void *context, size_t size)
{
    cmpp_esp32_platform_t *platform = context;
    return platform->allocator(platform->allocator_context, size);
}

static void platform_deallocate(void *context, void *memory)
{
    cmpp_esp32_platform_t *platform = context;
    platform->deallocator(platform->allocator_context, memory);
}

static uint64_t platform_monotonic_ms(void *context)
{
    (void)context;
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static cmpp_lock_handle_t platform_lock_create(void *context)
{
    (void)context;
    return (cmpp_lock_handle_t)xSemaphoreCreateMutex();
}

static void platform_lock_destroy(void *context, cmpp_lock_handle_t lock)
{
    (void)context;
    vSemaphoreDelete((SemaphoreHandle_t)lock);
}

static bool platform_lock_acquire(void *context, cmpp_lock_handle_t lock,
                                  uint32_t timeout_ms)
{
    (void)context;
    TickType_t ticks = portMAX_DELAY;
    if (timeout_ms != CMPP_WAIT_FOREVER) {
        ticks = pdMS_TO_TICKS(timeout_ms);
        if (timeout_ms > 0U && ticks == 0U) {
            ticks = 1U;
        }
    }
    return xSemaphoreTake((SemaphoreHandle_t)lock, ticks) == pdTRUE;
}

static void platform_lock_release(void *context, cmpp_lock_handle_t lock)
{
    (void)context;
    (void)xSemaphoreGive((SemaphoreHandle_t)lock);
}

static cmpp_status_t map_nvs_error(esp_err_t error)
{
    if (error == ESP_OK) {
        return CMPP_OK;
    }
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return CMPP_ERR_NOT_FOUND;
    }
    if (error == ESP_ERR_NVS_INVALID_LENGTH) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (error == ESP_ERR_NO_MEM) {
        return CMPP_ERR_NO_MEMORY;
    }
    return CMPP_ERR_IO;
}

static cmpp_status_t platform_storage_read(void *context, const char *key,
                                           void *data, size_t *size)
{
    cmpp_esp32_platform_t *platform = context;
    if (!key || !data || !size) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_open(platform->storage_namespace, NVS_READONLY,
                               &handle);
    if (error != ESP_OK) {
        return map_nvs_error(error);
    }
    error = nvs_get_blob(handle, key, data, size);
    nvs_close(handle);
    return map_nvs_error(error);
}

static cmpp_status_t platform_storage_write(void *context, const char *key,
                                            const void *data, size_t size)
{
    cmpp_esp32_platform_t *platform = context;
    if (!key || !data || size == 0U) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_open(platform->storage_namespace, NVS_READWRITE,
                               &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, key, data, size);
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    if (handle) {
        nvs_close(handle);
    }
    return map_nvs_error(error);
}

static cmpp_status_t platform_random_fill(void *context, void *data,
                                          size_t size)
{
    (void)context;
    if (!data && size > 0U) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    esp_fill_random(data, size);
    return CMPP_OK;
}

static void platform_log(void *context, cmpp_log_level_t level,
                         const char *message)
{
    (void)context;
    const char *text = message ? message : "";
    switch (level) {
    case CMPP_LOG_ERROR:
        ESP_LOGE("cmpp", "%s", text);
        break;
    case CMPP_LOG_WARN:
        ESP_LOGW("cmpp", "%s", text);
        break;
    case CMPP_LOG_DEBUG:
        ESP_LOGD("cmpp", "%s", text);
        break;
    case CMPP_LOG_INFO:
    default:
        ESP_LOGI("cmpp", "%s", text);
        break;
    }
}

cmpp_status_t cmpp_esp32_platform_create(
    const cmpp_esp32_platform_config_t *config,
    cmpp_esp32_platform_t **output)
{
    if (!output || (config && ((config->allocate && !config->deallocate) ||
                               (!config->allocate && config->deallocate)))) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    *output = NULL;
    void *(*allocator)(void *, size_t) =
        config && config->allocate ? config->allocate : default_allocate;
    void (*deallocator)(void *, void *) =
        config && config->deallocate ? config->deallocate : default_deallocate;
    void *allocator_context = config ? config->allocator_context : NULL;
    cmpp_esp32_platform_t *platform =
        allocator(allocator_context, sizeof(*platform));
    if (!platform) {
        return CMPP_ERR_NO_MEMORY;
    }
    memset(platform, 0, sizeof(*platform));
    platform->allocator = allocator;
    platform->deallocator = deallocator;
    platform->allocator_context = allocator_context;
    const char *storage_namespace =
        config && config->storage_namespace ? config->storage_namespace
                                            : DEFAULT_NAMESPACE;
    if (strlen(storage_namespace) == 0U ||
        strlen(storage_namespace) > NAMESPACE_MAX) {
        deallocator(allocator_context, platform);
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    (void)snprintf(platform->storage_namespace,
                   sizeof(platform->storage_namespace), "%s",
                   storage_namespace);
    platform->ops = (cmpp_platform_ops_t){
        .context = platform,
        .allocate = platform_allocate,
        .deallocate = platform_deallocate,
        .monotonic_ms = platform_monotonic_ms,
        .lock_create = platform_lock_create,
        .lock_destroy = platform_lock_destroy,
        .lock_acquire = platform_lock_acquire,
        .lock_release = platform_lock_release,
        .storage_read = platform_storage_read,
        .storage_write = platform_storage_write,
        .random_fill = platform_random_fill,
        .log = platform_log,
    };
    *output = platform;
    return CMPP_OK;
}

void cmpp_esp32_platform_destroy(cmpp_esp32_platform_t *platform)
{
    if (platform) {
        platform->deallocator(platform->allocator_context, platform);
    }
}

const cmpp_platform_ops_t *cmpp_esp32_platform_ops(
    cmpp_esp32_platform_t *platform)
{
    return platform ? &platform->ops : NULL;
}
