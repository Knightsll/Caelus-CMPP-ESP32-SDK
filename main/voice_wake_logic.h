/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "voice_wire.h"

typedef enum { VOICE_WAKE_OFF, VOICE_WAKE_NOISE, VOICE_WAKE_CAPTURE, VOICE_WAKE_FOLLOWUP, VOICE_WAKE_BARGE } voice_wake_mode_t;
typedef enum { VOICE_WAKE_NONE, VOICE_WAKE_START, VOICE_WAKE_END } voice_wake_event_t;

/* Counts delivered 20 ms frames, not inference wall time. */
typedef struct { unsigned elapsed_ms, speech_ms, silence_ms; } voice_endpoint_t;
bool voice_endpoint_step(voice_endpoint_t *state, bool speech, unsigned silence_ms);

typedef struct { bool active; int64_t deadline_ms; } voice_conversation_t;
/* Quiet timeout ends local follow-up listening, never the media connection.
 * Recording/thinking/playback suspend the timeout until the user can speak. */
bool voice_conversation_step(voice_conversation_t *state, bool ready, bool busy,
                             bool voice_started, int64_t now_ms, unsigned timeout_ms);
typedef struct { uint32_t history; unsigned consecutive_ms; } voice_onset_t;
bool voice_followup_step(voice_onset_t *state, bool speech, double power, double noise_power,
                         bool barge, unsigned barge_min_rms);

/* PTT can take over a voice-started turn without emitting another input.start. */
voice_ptt_event_t voice_input_step(voice_ptt_t *ptt, bool *wake_recording,
                                 bool ready, bool blocked, bool recording,
                                 bool pressed, bool reply_busy, voice_wake_event_t wake);

/* Caller applies readiness, stop and post-playback cooldown barriers. */
voice_wake_mode_t voice_listen_mode(bool recording, bool automatic, bool followup,
    bool reply_busy, bool gated, bool pressed, bool echo_ready);
