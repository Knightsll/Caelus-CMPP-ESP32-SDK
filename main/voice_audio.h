/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define VOICE_SAMPLE_RATE 16000
#define VOICE_FRAME_SAMPLES 320
#define VOICE_FRAME_BYTES (VOICE_FRAME_SAMPLES * sizeof(int16_t))
#define VOICE_DMA_DESCRIPTORS 8
#define VOICE_DMA_FRAMES 160
/* Drain more than one complete TX ring before reporting output.drained. */
#define VOICE_DRAIN_FRAMES ((VOICE_DMA_DESCRIPTORS * VOICE_DMA_FRAMES) / VOICE_FRAME_SAMPLES + 1)

esp_err_t voice_audio_init(void);
esp_err_t voice_audio_read(int16_t pcm[VOICE_FRAME_SAMPLES]);
esp_err_t voice_audio_write(const int16_t *pcm, size_t samples);
/* Nonblocking TX; a full DMA ring is success with a short/zero write. */
esp_err_t voice_audio_try_write(const int16_t *pcm, size_t samples, size_t *written);
esp_err_t voice_audio_mute(bool mute);
/* Caller serializes output writes: mute, discard TX DMA, keep microphone on. */
esp_err_t voice_audio_discard_output(void);
bool voice_audio_pressed(void);
bool voice_audio_stop_pressed(void);
