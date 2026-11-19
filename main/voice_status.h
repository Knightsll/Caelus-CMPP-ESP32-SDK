/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum {
    VOICE_LED_SETUP = 1U << 0,
    VOICE_LED_NETWORK = 1U << 1,
    VOICE_LED_READY = 1U << 2,
    VOICE_LED_RECORDING = 1U << 3,
    VOICE_LED_PLAYING = 1U << 4,
    VOICE_LED_FAULT = 1U << 5,
    VOICE_LED_WAITING = 1U << 6,
    VOICE_LED_MAINTENANCE = 1U << 7,
    VOICE_LED_MEDIA = VOICE_LED_READY | VOICE_LED_RECORDING | VOICE_LED_PLAYING | VOICE_LED_WAITING,
};

/* Actual state flags, not a timer pretending that a connection has succeeded. */
static inline bool voice_status_lit(unsigned flags, uint64_t ms)
{
    if (flags & VOICE_LED_MAINTENANCE) return true;
    if (flags & VOICE_LED_FAULT) return ms % 200 < 100;
    if (flags & VOICE_LED_SETUP) return ms % 1000 < 500;
    if (!(flags & VOICE_LED_NETWORK))
        return ms % 2000 < 100 || (ms % 2000 >= 200 && ms % 2000 < 300);
    if (!(flags & VOICE_LED_READY)) return false;
    if (flags & VOICE_LED_RECORDING) return true;
    if (flags & VOICE_LED_PLAYING) return ms % 500 < 250;
    if (flags & VOICE_LED_WAITING) return ms % 3000 < 1500;
    return ms % 2000 < 100;
}

#ifdef ESP_PLATFORM
#include "esp_err.h"
esp_err_t voice_status_init(void);
void voice_status_update(unsigned mask, unsigned value);
#endif
