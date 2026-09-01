/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/cmpp_backend_relay.h"

#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct cmpp_relay_backend {
    cmpp_relay_config_t config;
    SemaphoreHandle_t lock;
    bool current_on;
    uint64_t last_switch_ms;
};

static cmpp_status_t set_locked(cmpp_relay_backend_t *backend, bool on,
                                bool enforce_interval)
{
    if (on == backend->current_on) {
        return CMPP_OK;
    }
    const uint64_t now = (uint64_t)esp_timer_get_time() / 1000U;
    if (enforce_interval && backend->config.minimum_switch_interval_ms > 0U &&
        now - backend->last_switch_ms <
            backend->config.minimum_switch_interval_ms) {
        return CMPP_ERR_TIMEOUT;
    }
    if (gpio_set_level((gpio_num_t)backend->config.output_pin,
                       cmpp_relay_physical_level(backend->config.active_high,
                                                 on)) != ESP_OK) {
        return CMPP_ERR_IO;
    }
    backend->current_on = on;
    backend->last_switch_ms = now;
    return CMPP_OK;
}

cmpp_status_t cmpp_relay_create(const cmpp_relay_config_t *config,
                                cmpp_relay_backend_t **output)
{
    if (!config || !output || !GPIO_IS_VALID_OUTPUT_GPIO(config->output_pin)) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    *output = NULL;
    cmpp_relay_backend_t *backend = calloc(1U, sizeof(*backend));
    if (!backend) {
        return CMPP_ERR_NO_MEMORY;
    }
    backend->config = *config;
    backend->lock = xSemaphoreCreateMutex();
    if (!backend->lock) {
        free(backend);
        return CMPP_ERR_NO_MEMORY;
    }
    gpio_config_t gpio = {
        .pin_bit_mask = UINT64_C(1) << (unsigned)config->output_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&gpio) != ESP_OK ||
        gpio_set_level((gpio_num_t)config->output_pin,
                       cmpp_relay_physical_level(config->active_high,
                                                 config->boot_on)) != ESP_OK) {
        cmpp_relay_destroy(backend);
        return CMPP_ERR_IO;
    }
    backend->current_on = config->boot_on;
    backend->last_switch_ms = (uint64_t)esp_timer_get_time() / 1000U;
    *output = backend;
    return CMPP_OK;
}

void cmpp_relay_destroy(cmpp_relay_backend_t *backend)
{
    if (backend) {
        (void)gpio_set_level((gpio_num_t)backend->config.output_pin,
                             cmpp_relay_physical_level(
                                 backend->config.active_high,
                                 backend->config.safe_stop_on));
        (void)gpio_reset_pin((gpio_num_t)backend->config.output_pin);
        if (backend->lock) {
            vSemaphoreDelete(backend->lock);
        }
        free(backend);
    }
}

cmpp_status_t cmpp_relay_set(cmpp_relay_backend_t *backend, bool on)
{
    if (!backend) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (xSemaphoreTake(backend->lock, portMAX_DELAY) != pdTRUE) {
        return CMPP_ERR_TIMEOUT;
    }
    cmpp_status_t status = set_locked(backend, on, true);
    xSemaphoreGive(backend->lock);
    return status;
}

cmpp_status_t cmpp_relay_safe_stop(cmpp_relay_backend_t *backend)
{
    if (!backend) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (xSemaphoreTake(backend->lock, portMAX_DELAY) != pdTRUE) {
        return CMPP_ERR_TIMEOUT;
    }
    cmpp_status_t status = set_locked(backend, backend->config.safe_stop_on,
                                      false);
    xSemaphoreGive(backend->lock);
    return status;
}

cmpp_status_t cmpp_relay_get_state(cmpp_relay_backend_t *backend, bool *on)
{
    if (!backend || !on) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (xSemaphoreTake(backend->lock, portMAX_DELAY) != pdTRUE) {
        return CMPP_ERR_TIMEOUT;
    }
    *on = backend->current_on;
    xSemaphoreGive(backend->lock);
    return CMPP_OK;
}
