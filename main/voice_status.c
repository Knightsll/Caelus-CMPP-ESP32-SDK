/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_status.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"

static unsigned s_flags;
static const char *TAG = "voice_led";

static void tick(void *arg)
{
    (void)arg;
    unsigned flags = __atomic_load_n(&s_flags, __ATOMIC_RELAXED);
    bool on = voice_status_lit(flags, (uint64_t)(esp_timer_get_time() / 1000));
    gpio_set_level(GPIO_NUM_46, on ? 0 : 1);
}

void voice_status_update(unsigned mask, unsigned value)
{
    unsigned previous = __atomic_load_n(&s_flags, __ATOMIC_RELAXED), next;
    do {
        next = (previous & ~mask) | (value & mask);
    } while (!__atomic_compare_exchange_n(&s_flags, &previous, next, false,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    if (next != previous) ESP_LOGI(TAG, "State flags=0x%02x", next);
}

esp_err_t voice_status_init(void)
{
    /* Strapping has already been sampled. Never change eFuses or pulls here.
     * LED1: 3V3 -> R64 3k -> LED -> GPIO46, active low. */
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_46, 1), TAG, "LED latch failed");
    gpio_config_t config = {.pin_bit_mask = 1ULL << 46, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "LED GPIO init failed");
    const esp_timer_create_args_t args = {.callback = tick, .name = "voice_led"};
    esp_timer_handle_t timer;
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &timer), TAG, "LED timer failed");
    return esp_timer_start_periodic(timer, 50000);
}
