/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define VOICE_SAMPLE_RATE 16000
#define VOICE_FRAME_SAMPLES 320
#define VOICE_FRAME_BYTES (VOICE_FRAME_SAMPLES * sizeof(int16_t))

esp_err_t voice_audio_init(void);
esp_err_t voice_audio_read(int16_t pcm[VOICE_FRAME_SAMPLES]);
esp_err_t voice_audio_write(const int16_t *pcm, size_t samples);
esp_err_t voice_audio_mute(bool mute);
/* Caller serializes output writes: mute, discard TX DMA, keep microphone on. */
esp_err_t voice_audio_discard_output(void);
bool voice_audio_pressed(void);
