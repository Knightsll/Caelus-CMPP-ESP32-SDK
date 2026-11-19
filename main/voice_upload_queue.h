/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Match XiaoZhi's 2.4 s realtime backlog duration, not its compressed byte
 * budget. This PCM bridge needs 76,800 payload bytes; measure heap on target. */
#define VOICE_UPLOAD_PCM_FRAMES 120
#define VOICE_UPLOAD_CONTROL_SLOTS 4
#define VOICE_UPLOAD_SLOTS (VOICE_UPLOAD_PCM_FRAMES + VOICE_UPLOAD_CONTROL_SLOTS)
#define VOICE_UPLOAD_PACKET_BYTES 640

typedef struct {
    uint32_t generation;
    size_t bytes;
    bool text;
    int16_t pcm[VOICE_UPLOAD_PACKET_BYTES / sizeof(int16_t)];
} voice_packet_t;

/* Caller owns synchronization. Index movement never copies the PCM backlog. */
typedef struct {
    voice_packet_t packets[VOICE_UPLOAD_SLOTS];
    uint16_t order[VOICE_UPLOAD_SLOTS];
    bool used[VOICE_UPLOAD_SLOTS];
    bool work_pending;
    size_t count, pcm_count;
} voice_upload_queue_t;

void voice_upload_queue_reset(voice_upload_queue_t *q);
/* A full audio queue replaces only its oldest PCM. Text is never evicted.
 * dropped_bytes is zero on a lossless push. False means invalid/full control. */
bool voice_upload_queue_push(voice_upload_queue_t *q, const voice_packet_t *packet,
                             size_t *dropped_bytes);
bool voice_upload_queue_pop(voice_upload_queue_t *q, voice_packet_t *packet);
/* One scheduled/in-flight work item at a time, including across queue reset. */
int voice_upload_queue_schedule(voice_upload_queue_t *q, int (*submit)(void *), void *arg);
