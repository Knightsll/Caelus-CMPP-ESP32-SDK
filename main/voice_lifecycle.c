/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_lifecycle.h"

void voice_lifecycle_open(voice_lifecycle_t *s)
{
    ++s->generation;
    s->active = true;
    /* Opening alone cannot prove recovery; only a valid hello clears fault. */
}

bool voice_lifecycle_accept(const voice_lifecycle_t *s, uint32_t generation)
{
    return s->active && s->generation == generation;
}

bool voice_lifecycle_stop(voice_lifecycle_t *s, voice_stop_reason_t reason)
{
    if (!s->active) return false;
    s->active = false;
    ++s->generation;
    s->last_stop_reason = reason;
    if (reason >= VOICE_STOP_SEND_FAILED) {
        s->fault = true;
        s->last_fault_reason = reason;
    }
    return true;
}

void voice_lifecycle_audio_stop_failed(voice_lifecycle_t *s)
{
    s->fault = true;
    s->last_fault_reason = VOICE_STOP_AUDIO_STOP_FAILED;
}

void voice_lifecycle_ready(voice_lifecycle_t *s)
{
    if (s->active) s->fault = false;
    /* Retain the last actual failure after recovery for read-only diagnosis. */
}

const char *voice_stop_reason_name(voice_stop_reason_t reason)
{
    switch (reason) {
    case VOICE_STOP_NONE: return "none";
    case VOICE_STOP_REQUESTED: return "requested";
    case VOICE_STOP_PEER_CLOSED: return "peer_closed";
    case VOICE_STOP_NETWORK_LOST: return "network_lost";
    case VOICE_STOP_HELLO_TIMEOUT: return "hello_timeout";
    case VOICE_STOP_HEARTBEAT_TIMEOUT: return "heartbeat_timeout";
    case VOICE_STOP_OPEN_TIMEOUT: return "open_timeout";
    case VOICE_STOP_SEND_FAILED: return "send_failed";
    case VOICE_STOP_RECEIVE_FAILED: return "receive_failed";
    case VOICE_STOP_CAPTURE_FAILED: return "capture_failed";
    case VOICE_STOP_UPLOAD_FAILED: return "upload_failed";
    case VOICE_STOP_UPLOAD_QUEUE_FULL: return "upload_queue_full";
    case VOICE_STOP_UPLOAD_CONTROL_FULL: return "upload_control_queue_full";
    case VOICE_STOP_UPLOAD_WORK_FAILED: return "upload_work_schedule_failed";
    case VOICE_STOP_PLAYBACK_FAILED: return "playback_failed";
    case VOICE_STOP_PLAYBACK_QUEUE_FULL: return "playback_queue_full";
    case VOICE_STOP_INVALID_FRAME: return "invalid_frame";
    case VOICE_STOP_STATE_CONFLICT: return "state_conflict";
    case VOICE_STOP_AUDIO_STOP_FAILED: return "audio_stop_failed";
    default: return "unknown";
    }
}
