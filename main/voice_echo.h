/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "voice_audio.h"
typedef struct {
    bool available, ready;
    /* Input meters are delayed with output by the bridge for comparison. */
    unsigned mic_rms, reference_rms, output_rms, frame_samples, worst_us, average_us;
} voice_echo_stats_t;
bool voice_echo_init(void);
/* Single capture-task owner; microphone and reference must be synchronous. */
bool voice_echo_process(const int16_t *mic, const int16_t *reference, int16_t *output);
voice_echo_stats_t voice_echo_stats(void);
