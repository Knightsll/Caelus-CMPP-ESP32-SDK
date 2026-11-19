/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    VOICE_STOP_NONE, VOICE_STOP_REQUESTED, VOICE_STOP_PEER_CLOSED,
    VOICE_STOP_NETWORK_LOST, VOICE_STOP_HELLO_TIMEOUT, VOICE_STOP_HEARTBEAT_TIMEOUT,
    VOICE_STOP_OPEN_TIMEOUT, VOICE_STOP_SEND_FAILED, VOICE_STOP_RECEIVE_FAILED,
    VOICE_STOP_CAPTURE_FAILED, VOICE_STOP_UPLOAD_FAILED, VOICE_STOP_PLAYBACK_FAILED,
    VOICE_STOP_PLAYBACK_QUEUE_FULL, VOICE_STOP_INVALID_FRAME,
    VOICE_STOP_STATE_CONFLICT, VOICE_STOP_AUDIO_STOP_FAILED,
    VOICE_STOP_UPLOAD_QUEUE_FULL, VOICE_STOP_UPLOAD_WORK_FAILED,
    VOICE_STOP_UPLOAD_CONTROL_FULL,
} voice_stop_reason_t;

typedef struct {
    uint32_t generation;
    bool active, fault;
    voice_stop_reason_t last_stop_reason, last_fault_reason;
} voice_lifecycle_t;

void voice_lifecycle_open(voice_lifecycle_t *state);
bool voice_lifecycle_accept(const voice_lifecycle_t *state, uint32_t generation);
/* False means cleanup already began: late frames/callbacks must not relabel it. */
bool voice_lifecycle_stop(voice_lifecycle_t *state, voice_stop_reason_t reason);
void voice_lifecycle_audio_stop_failed(voice_lifecycle_t *state);
void voice_lifecycle_ready(voice_lifecycle_t *state);
const char *voice_stop_reason_name(voice_stop_reason_t reason);
