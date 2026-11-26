/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VOICE_PROTOCOL "caelus.voice.v1"
#define VOICE_ID_MAX 64
#define VOICE_PCM_BYTES 640
#define VOICE_JSON_BYTES 512
#define VOICE_INTERRUPT_FEATURE "ptt_interrupt_v1"
#define VOICE_STOP_FEATURE "session_stop_v1"

typedef enum {
    VOICE_BAD, VOICE_HELLO, VOICE_OUTPUT_START, VOICE_OUTPUT_END,
    VOICE_SESSION_THINKING, VOICE_SESSION_LISTENING,
    VOICE_CONFIGURED, VOICE_INPUT_ACCEPTED, VOICE_IGNORED, VOICE_SESSION_STOPPED,
} voice_message_t;
typedef struct {
    bool ready;
    bool output;
    bool tail;
    bool interrupt_enabled, input_active, awaiting_accept;
    bool stop_enabled, awaiting_stop;
    char stop_id[VOICE_ID_MAX + 1];
    char input_turn[VOICE_ID_MAX + 1];
    char turn_id[VOICE_ID_MAX + 1];
} voice_wire_t;

bool voice_id_valid(const char *id);
voice_message_t voice_wire_text(voice_wire_t *state, const char *channel_id,
                                const char *json, size_t size);
bool voice_wire_pcm(voice_wire_t *state, size_t size);
/* Called locally at PTT edges. Capture never waits for the downlink barrier. */
bool voice_wire_begin_input(voice_wire_t *state, const char *turn_id);
bool voice_wire_begin_stop(voice_wire_t *state, const char *stop_id);
void voice_wire_end_input(voice_wire_t *state);
bool voice_wire_output_blocked(const voice_wire_t *state);

typedef struct { bool armed, previous_pressed; } voice_ptt_t;
typedef enum { VOICE_PTT_NONE, VOICE_PTT_START, VOICE_PTT_END } voice_ptt_event_t;
voice_ptt_event_t voice_ptt_step(voice_ptt_t *state, bool ready, bool output_busy,
                                bool recording, bool pressed);
