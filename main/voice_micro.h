/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct voice_micro voice_micro_t;
const char *voice_micro_create(voice_micro_t **out);
void voice_micro_destroy(voice_micro_t *model);
bool voice_micro_reset(voice_micro_t *model);
/* 1 = wake, 0 = no wake, -1 = inference failure. Single worker owner. */
int voice_micro_process(voice_micro_t *model, const int16_t *pcm, size_t count);
#ifdef __cplusplus
}
#endif
