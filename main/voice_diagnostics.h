/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include "voice_lifecycle.h"

/* Boot-lifetime aggregates, retained across reconnects; no PCM or credentials. */
typedef struct {
    uint32_t upload_pcm_queued_bytes, upload_pcm_sent_bytes;
    uint32_t output_pcm_received_bytes, output_pcm_i2s_bytes;
    uint32_t upload_queue_high_water, upload_queue_full_count;
    uint32_t upload_pcm_dropped_frames, upload_pcm_dropped_bytes;
    uint32_t upload_control_queue_full_count;
    uint32_t upload_work_schedule_failed_count, upload_send_failed_count;
    uint32_t upload_send_max_us, last_upload_failure_generation;
    int last_upload_schedule_error, last_upload_schedule_errno;
    const char *last_upload_failure_packet;
    voice_stop_reason_t last_upload_failure;
} voice_diagnostics_t;

static inline void voice_diag_add(uint32_t *counter, uint32_t amount)
{
    *counter = amount > UINT32_MAX - *counter ? UINT32_MAX : *counter + amount;
}

static inline void voice_diag_send(voice_diagnostics_t *d, uint64_t elapsed_us,
                                   uint32_t pcm_bytes, int failed)
{
    uint32_t duration = elapsed_us > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_us;
    if (duration > d->upload_send_max_us) d->upload_send_max_us = duration;
    if (failed) voice_diag_add(&d->upload_send_failed_count, 1);
    else voice_diag_add(&d->upload_pcm_sent_bytes, pcm_bytes);
}

static inline voice_stop_reason_t voice_diag_upload_failure(
    voice_diagnostics_t *d, voice_stop_reason_t reason, const char *packet,
    uint32_t generation, int error, int saved_errno)
{
    if (reason == VOICE_STOP_UPLOAD_CONTROL_FULL) voice_diag_add(&d->upload_control_queue_full_count, 1);
    if (reason == VOICE_STOP_UPLOAD_QUEUE_FULL) voice_diag_add(&d->upload_queue_full_count, 1);
    if (reason == VOICE_STOP_UPLOAD_WORK_FAILED) {
        voice_diag_add(&d->upload_work_schedule_failed_count, 1);
        d->last_upload_schedule_error = error;
        d->last_upload_schedule_errno = saved_errno;
    }
    d->last_upload_failure = reason;
    d->last_upload_failure_packet = packet;
    d->last_upload_failure_generation = generation;
    return reason;
}
