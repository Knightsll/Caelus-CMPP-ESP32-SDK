/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "voice_wire.h"

static voice_message_t message(voice_wire_t *state, const char *json)
{
    return voice_wire_text(state, "paired_channel", json, strlen(json));
}

static void interrupt_protocol(void)
{
    voice_wire_t s = {0};
    assert(message(&s, "{\"type\":\"session.configure\",\"interrupt\":true}") == VOICE_BAD);
    assert(message(&s, "{\"type\":\"hello\",\"protocol\":\"caelus.voice.v1\",\"channel_id\":\"paired_channel\"}") == VOICE_HELLO);
    assert(!s.interrupt_enabled); /* Old hosts keep the existing half-duplex behavior. */
    assert(message(&s, "{\"type\":\"input.accepted\",\"turn_id\":\"in_1\"}") == VOICE_BAD);
    assert(message(&s, "{\"type\":\"session.configure\",\"interrupt\":1}") == VOICE_BAD);
    assert(message(&s, "{\"type\":\"session.configure\",\"interrupt\":true,\"extra\":0}") == VOICE_BAD);
    assert(message(&s, "{\"type\":\"session.configure\",\"interrupt\":true}") == VOICE_CONFIGURED);
    assert(s.interrupt_enabled);
    assert(message(&s, "{\"type\":\"output.start\",\"turn_id\":\"old\"}") == VOICE_OUTPUT_START);
    assert(voice_wire_pcm(&s, 640));
    assert(voice_wire_begin_input(&s, "in_1")); /* Local PTT, before any host reply. */
    assert(s.input_active && s.awaiting_accept && !s.output);
    assert(voice_wire_output_blocked(&s));
    assert(message(&s, "{\"type\":\"output.end\",\"turn_id\":\"old\"}") == VOICE_IGNORED);
    assert(message(&s, "{\"type\":\"output.start\",\"turn_id\":\"late_old_part\"}") == VOICE_IGNORED);
    assert(message(&s, "{\"type\":\"session.state\",\"state\":\"listening\"}") == VOICE_IGNORED);
    assert(!s.output && !voice_wire_pcm(&s, 640));
    assert(message(&s, "{\"type\":\"input.accepted\",\"turn_id\":\"wrong\"}") == VOICE_IGNORED);
    assert(s.awaiting_accept);
    assert(message(&s, "{\"type\":\"input.accepted\",\"turn_id\":\"in_1\"}") == VOICE_INPUT_ACCEPTED);
    assert(!s.awaiting_accept && s.input_active && voice_wire_output_blocked(&s));
    assert(message(&s, "{\"type\":\"session.state\",\"state\":\"thinking\"}") == VOICE_IGNORED);
    voice_wire_end_input(&s);
    assert(!voice_wire_output_blocked(&s));
    assert(message(&s, "{\"type\":\"output.start\",\"turn_id\":\"new\"}") == VOICE_OUTPUT_START);
    assert(voice_wire_pcm(&s, 640));
    /* Another interruption and release before ACK: no stale ACK unlocks it. */
    assert(voice_wire_begin_input(&s, "in_2"));
    voice_wire_end_input(&s);
    assert(voice_wire_output_blocked(&s));
    assert(message(&s, "{\"type\":\"input.accepted\",\"turn_id\":\"in_1\"}") == VOICE_IGNORED);
    assert(voice_wire_begin_input(&s, "in_3"));
    assert(message(&s, "{\"type\":\"input.accepted\",\"turn_id\":\"in_2\"}") == VOICE_IGNORED);
    voice_wire_end_input(&s);
    assert(voice_wire_output_blocked(&s));
    assert(message(&s, "{\"type\":\"input.accepted\",\"turn_id\":\"in_3\"}") == VOICE_INPUT_ACCEPTED);
    assert(!voice_wire_output_blocked(&s));
    assert(message(&s, "{\"type\":\"output.start\",\"turn_id\":\"third\"}") == VOICE_OUTPUT_START);
    assert(voice_wire_pcm(&s, 320));
    assert(message(&s, "{\"type\":\"output.end\",\"turn_id\":\"third\"}") == VOICE_OUTPUT_END);
    assert(message(&s, "{\"type\":\"session.state\",\"state\":\"listening\"}") == VOICE_SESSION_LISTENING);
    memset(&s, 0, sizeof(s));
    assert(!s.interrupt_enabled && !s.input_active && !s.awaiting_accept);
    assert(!voice_wire_begin_input(&s, "in_4")); /* Disconnected PTT never starts. */
}

int main(void)
{
    interrupt_protocol();
    voice_ptt_t ptt = {0};
    assert(voice_ptt_step(&ptt, false, false, false, true) == VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt, true, false, false, true) == VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt, true, false, false, false) == VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt, true, false, false, true) == VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt, true, false, false, true) == VOICE_PTT_START);
    assert(voice_ptt_step(&ptt, true, false, true, false) == VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt, true, false, true, false) == VOICE_PTT_END);
    assert(voice_ptt_step(&ptt, true, true, false, true) == VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt, true, true, false, true) == VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt, true, false, false, true) == VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt, true, false, false, true) == VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt, true, false, false, false) == VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt, true, false, false, true) == VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt, true, false, false, true) == VOICE_PTT_START);
    voice_wire_t state = {0};
    assert(!voice_wire_pcm(&state, 640));
    assert(message(&state, "{\"type\":\"hello\",\"protocol\":\"caelus.voice.v1\",\"channel_id\":\"wrong\"}") == VOICE_BAD);
    assert(!state.ready);
    assert(message(&state, "{\"type\":\"session.state\",\"state\":\"listening\"}") == VOICE_BAD);
    assert(message(&state, "{\"type\":\"hello\",\"protocol\":\"caelus.voice.v1\",\"channel_id\":\"paired_channel\"}") == VOICE_HELLO);
    assert(message(&state, "{\"type\":\"session.state\",\"state\":\"thinking\"}") == VOICE_SESSION_THINKING);
    assert(message(&state, "{\"type\":\"session.state\",\"state\":\"listening\"}") == VOICE_SESSION_LISTENING);
    assert(message(&state, "{\"type\":\"session.state\",\"state\":\"unknown\"}") == VOICE_BAD);
    assert(message(&state, "{\"type\":\"session.state\",\"state\":\"listening\",\"extra\":0}") == VOICE_BAD);
    assert(!voice_wire_pcm(&state, 640));
    assert(message(&state, "{\"type\":\"output.start\",\"turn_id\":\"one\"}garbage") == VOICE_BAD);
    assert(message(&state, "{\"type\":\"output.start\",\"turn_id\":\"one\"}") == VOICE_OUTPUT_START);
    assert(message(&state, "{\"type\":\"output.start\",\"turn_id\":\"two\"}") == VOICE_BAD);
    assert(!voice_wire_pcm(&state, 0));
    assert(!voice_wire_pcm(&state, 641));
    assert(!voice_wire_pcm(&state, 639));
    assert(voice_wire_pcm(&state, 640));
    assert(voice_wire_pcm(&state, 320));
    assert(!voice_wire_pcm(&state, 640)); /* Short frame must be the tail. */
    assert(message(&state, "{\"type\":\"output.end\",\"turn_id\":\"wrong\"}") == VOICE_BAD);
    assert(message(&state, "{\"type\":\"output.end\",\"turn_id\":\"one\"}") == VOICE_OUTPUT_END);
    assert(!voice_wire_pcm(&state, 640));
    assert(message(&state, "{\"type\":\"output.start\",\"turn_id\":\"one\"}") == VOICE_BAD);
    assert(message(&state, "{\"type\":\"output.start\",\"turn_id\":\"two\"}") == VOICE_OUTPUT_START);
    assert(!voice_id_valid(""));
    assert(!voice_id_valid("untrusted\"id"));
    assert(!voice_id_valid("room/path"));
    assert(voice_id_valid("uuid-123_abc"));
    char long_id[66];
    memset(long_id, 'a', 65);
    long_id[65] = 0;
    assert(!voice_id_valid(long_id));
    memset(&state, 0, sizeof(state)); /* Reconnection cannot resume old media. */
    assert(message(&state, "{\"type\":\"output.end\",\"turn_id\":\"two\"}") == VOICE_BAD);
    assert(message(&state, "[]") == VOICE_BAD);
    assert(message(&state, "{\"type\":\"hello\",\"type\":\"hello\",\"protocol\":\"caelus.voice.v1\",\"channel_id\":\"paired_channel\"}") == VOICE_BAD);
    state = (voice_wire_t){.ready = true};
    assert(!voice_wire_begin_stop(&state, "stop_1"));
    assert(message(&state, "{\"type\":\"session.configure\",\"interrupt\":true,\"session_stop\":true}") == VOICE_CONFIGURED);
    assert(voice_wire_begin_input(&state, "input_before_stop"));
    assert(voice_wire_begin_stop(&state, "stop_1"));
    assert(voice_wire_output_blocked(&state));
    assert(!voice_wire_begin_input(&state, "too_early"));
    assert(message(&state, "{\"type\":\"output.start\",\"turn_id\":\"old_reply\"}") == VOICE_IGNORED);
    assert(message(&state, "{\"type\":\"session.state\",\"state\":\"listening\"}") == VOICE_IGNORED);
    assert(message(&state, "{\"type\":\"session.state\",\"state\":\"listening\",\"stop_id\":\"stale\"}") == VOICE_IGNORED);
    assert(voice_wire_output_blocked(&state));
    assert(message(&state, "{\"type\":\"session.state\",\"state\":\"listening\",\"stop_id\":\"stop_1\"}") == VOICE_SESSION_STOPPED);
    assert(!voice_wire_output_blocked(&state));
    assert(message(&state, "{\"type\":\"session.state\",\"state\":\"listening\",\"stop_id\":\"stop_1\"}") == VOICE_IGNORED);
    assert(voice_wire_begin_input(&state, "next_wake"));
    puts("voice wire: session binding, stop barrier, boundaries, tails and malformed messages passed");
    return 0;
}
