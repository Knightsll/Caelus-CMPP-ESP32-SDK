/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/cmpp_backend_pwm.h"

#include <stdlib.h>

#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct cmpp_pwm_backend {
    cmpp_pwm_config_t config;
    SemaphoreHandle_t lock;
    uint16_t current_output;
    uint32_t maximum_duty;
};

static cmpp_status_t apply_output(cmpp_pwm_backend_t *backend,
                                  uint16_t normalized)
{
    uint32_t duty = 0U;
    cmpp_status_t map_status = cmpp_pwm_map_duty(
        backend->config.resolution_bits, backend->config.inverted, normalized,
        &duty);
    if (map_status != CMPP_OK) {
        return map_status;
    }
    esp_err_t error = ledc_set_duty(LEDC_LOW_SPEED_MODE,
                                    (ledc_channel_t)backend->config.channel,
                                    duty);
    if (error == ESP_OK) {
        error = ledc_update_duty(LEDC_LOW_SPEED_MODE,
                                 (ledc_channel_t)backend->config.channel);
    }
    if (error != ESP_OK) {
        return CMPP_ERR_IO;
    }
    backend->current_output = normalized;
    return CMPP_OK;
}

cmpp_status_t cmpp_pwm_create(const cmpp_pwm_config_t *config,
                              cmpp_pwm_backend_t **output)
{
    if (!config || !output || !cmpp_pwm_config_is_consistent(config) ||
        config->channel >= LEDC_CHANNEL_MAX ||
        config->timer >= LEDC_TIMER_MAX) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    *output = NULL;
    cmpp_pwm_backend_t *backend = calloc(1U, sizeof(*backend));
    if (!backend) {
        return CMPP_ERR_NO_MEMORY;
    }
    backend->config = *config;
    backend->maximum_duty = (1U << config->resolution_bits) - 1U;
    backend->lock = xSemaphoreCreateMutex();
    if (!backend->lock) {
        free(backend);
        return CMPP_ERR_NO_MEMORY;
    }
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = (ledc_timer_bit_t)config->resolution_bits,
        .timer_num = (ledc_timer_t)config->timer,
        .freq_hz = config->frequency_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        cmpp_pwm_destroy(backend);
        return CMPP_ERR_IO;
    }
    uint32_t startup_duty = 0U;
    (void)cmpp_pwm_map_duty(config->resolution_bits, config->inverted,
                            config->startup_output, &startup_duty);
    ledc_channel_config_t channel = {
        .gpio_num = config->output_pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)config->channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = (ledc_timer_t)config->timer,
        .duty = startup_duty,
        .hpoint = 0,
        .flags.output_invert = 0,
    };
    if (ledc_channel_config(&channel) != ESP_OK) {
        cmpp_pwm_destroy(backend);
        return CMPP_ERR_IO;
    }
    backend->current_output = config->startup_output;
    *output = backend;
    return CMPP_OK;
}

void cmpp_pwm_destroy(cmpp_pwm_backend_t *backend)
{
    if (backend) {
        (void)ledc_stop(LEDC_LOW_SPEED_MODE,
                        (ledc_channel_t)backend->config.channel,
                        backend->config.inverted ? 1U : 0U);
        if (backend->lock) {
            vSemaphoreDelete(backend->lock);
        }
        free(backend);
    }
}

cmpp_status_t cmpp_pwm_set(cmpp_pwm_backend_t *backend,
                           uint16_t normalized_output,
                           uint32_t transition_ms)
{
    if (!backend || normalized_output < backend->config.minimum_output ||
        normalized_output > backend->config.maximum_output) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (xSemaphoreTake(backend->lock, portMAX_DELAY) != pdTRUE) {
        return CMPP_ERR_TIMEOUT;
    }
    cmpp_status_t status = CMPP_OK;
    if (transition_ms == 0U || backend->config.ramp_step == 0U ||
        backend->current_output == normalized_output) {
        status = apply_output(backend, normalized_output);
    } else {
        uint16_t current = backend->current_output;
        const uint32_t difference = current < normalized_output
                                        ? normalized_output - current
                                        : current - normalized_output;
        uint32_t steps = (difference + backend->config.ramp_step - 1U) /
                         backend->config.ramp_step;
        uint32_t interval = transition_ms / (steps ? steps : 1U);
        if (interval == 0U) {
            interval = backend->config.ramp_interval_ms;
        }
        while (current != normalized_output && status == CMPP_OK) {
            if (current < normalized_output) {
                uint32_t next = current + backend->config.ramp_step;
                current = next > normalized_output ? normalized_output
                                                   : (uint16_t)next;
            } else {
                current = current - normalized_output <= backend->config.ramp_step
                              ? normalized_output
                              : (uint16_t)(current - backend->config.ramp_step);
            }
            status = apply_output(backend, current);
            if (status == CMPP_OK && current != normalized_output) {
                TickType_t ticks = pdMS_TO_TICKS(interval);
                vTaskDelay(ticks > 0U ? ticks : 1U);
            }
        }
    }
    xSemaphoreGive(backend->lock);
    return status;
}

cmpp_status_t cmpp_pwm_safe_stop(cmpp_pwm_backend_t *backend)
{
    if (!backend) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (xSemaphoreTake(backend->lock, portMAX_DELAY) != pdTRUE) {
        return CMPP_ERR_TIMEOUT;
    }
    cmpp_status_t status = apply_output(backend,
                                        backend->config.safe_stop_output);
    xSemaphoreGive(backend->lock);
    return status;
}

cmpp_status_t cmpp_pwm_get_output(cmpp_pwm_backend_t *backend,
                                  uint16_t *normalized_output)
{
    if (!backend || !normalized_output) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    if (xSemaphoreTake(backend->lock, portMAX_DELAY) != pdTRUE) {
        return CMPP_ERR_TIMEOUT;
    }
    *normalized_output = backend->current_output;
    xSemaphoreGive(backend->lock);
    return CMPP_OK;
}
