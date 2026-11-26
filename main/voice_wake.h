/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "voice_audio.h"
#include "voice_wake_logic.h"

typedef struct voice_wake voice_wake_t;
/* Failure leaves *output NULL: PTT remains usable. Status is a static string. */
const char *voice_wake_create(voice_wake_t **output);
void voice_wake_destroy(voice_wake_t *wake);
void voice_wake_set_barge_min_rms(voice_wake_t *wake, unsigned rms);
/* Only the capture task calls step. Nonblocking; inference runs separately.
 * Generation/mode changes and queue gaps invalidate stale recognition. */
voice_wake_event_t voice_wake_step(voice_wake_t *wake, voice_wake_mode_t mode,
                                  uint32_t generation, const int16_t *pcm);
