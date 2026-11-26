/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "voice_wake_logic.h"

int main(void)
{
    voice_conversation_t conversation = {0};
    assert(!voice_conversation_step(&conversation, true, false, false, 0, 30000));
    assert(voice_conversation_step(&conversation, true, false, true, 100, 30000));
    /* A long reply must not exhaust the window before the user can answer. */
    assert(voice_conversation_step(&conversation, true, true, false, 60000, 30000));
    assert(voice_conversation_step(&conversation, true, false, false, 89999, 30000));
    /* A follow-up starts another quiet window without requiring another wake. */
    assert(voice_conversation_step(&conversation, true, false, true, 89999, 30000));
    assert(voice_conversation_step(&conversation, true, false, false, 119998, 30000));
    assert(!voice_conversation_step(&conversation, true, false, false, 119999, 30000));
    assert(voice_conversation_step(&conversation, true, false, true, 120000, 30000));
    assert(!voice_conversation_step(&conversation, false, true, false, 120001, 30000));
    assert(!voice_conversation_step(&conversation, true, false, false, 120002, 30000));
    voice_onset_t onset = {0};
    for (int i = 0; i < 1500; ++i) {
        assert(!voice_followup_step(&onset, false, 100000, 6400, false, 1000));
        assert(!voice_followup_step(&onset, true, 6300, 1, false, 1000));
        assert(!voice_followup_step(&onset, true, 25599, 6400, false, 1000));
    }
    /* Two 100ms syllables separated by a 20ms weak consonant: old contiguous
     * gate misses this indefinitely; the bounded window must accept it. */
    for (int i = 0; i < 5; ++i) assert(!voice_followup_step(&onset, true, 100000, 6400, false, 1000));
    assert(!voice_followup_step(&onset, true, 100, 6400, false, 1000));
    for (int i = 0; i < 4; ++i) assert(!voice_followup_step(&onset, true, 100000, 6400, false, 1000));
    assert(voice_followup_step(&onset, true, 100000, 6400, false, 1000));
    onset = (voice_onset_t){0};
    /* Repeated isolated clicks cannot accumulate across the 400ms window. */
    for (int i = 0; i < 1500; ++i)
        assert(!voice_followup_step(&onset, i % 5 == 0, 100000, 6400, false, 1000));
    onset = (voice_onset_t){0};
    for (int i = 0; i < 14; ++i) assert(!voice_followup_step(&onset, true, 2000000, 6400, true, 1000));
    assert(!voice_followup_step(&onset, false, 100000, 6400, true, 1000));
    for (int i = 0; i < 14; ++i) assert(!voice_followup_step(&onset, true, 2000000, 6400, true, 1000));
    assert(voice_followup_step(&onset, true, 2000000, 6400, true, 1000));
    onset = (voice_onset_t){0};
    /* Playback threshold is independent from the ordinary 80 RMS gate. */
    for (int i = 0; i < 100; ++i)
        assert(!voice_followup_step(&onset, true, 999999, 6400, true, 1000));
    for (int i = 0; i < 14; ++i)
        assert(!voice_followup_step(&onset, true, 1000000, 6400, true, 1000));
    assert(voice_followup_step(&onset, true, 1000000, 6400, true, 1000));
    conversation = (voice_conversation_t){0};
    assert(voice_conversation_step(&conversation, true, false, true, 0, 15000));
    assert(voice_conversation_step(&conversation, true, false, false, 14999, 15000));
    assert(!voice_conversation_step(&conversation, true, false, false, 15000, 15000));
    voice_endpoint_t endpoint = {0};
    /* No command: close after 5 seconds; transient noise is not a command. */
    for (int i = 0; i < 249; ++i) assert(!voice_endpoint_step(&endpoint, i == 2, 1200));
    assert(voice_endpoint_step(&endpoint, false, 1200));
    endpoint = (voice_endpoint_t){0};
    for (int i = 0; i < 25; ++i) assert(!voice_endpoint_step(&endpoint, true, 1200));
    for (int i = 0; i < 59; ++i) assert(!voice_endpoint_step(&endpoint, false, 1200));
    assert(voice_endpoint_step(&endpoint, false, 1200));
    /* An intra-sentence pause resets on more speech. */
    endpoint = (voice_endpoint_t){0};
    for (int i = 0; i < 749; ++i) assert(!voice_endpoint_step(&endpoint, i % 40 == 0, 1200));
    assert(voice_endpoint_step(&endpoint, true, 1200));

    voice_ptt_t ptt = {0};
    bool wake = false;
    assert(voice_input_step(&ptt, &wake, false, false, false, false, false, VOICE_WAKE_START) == VOICE_PTT_NONE);
    assert(voice_input_step(&ptt, &wake, true, true, false, false, false, VOICE_WAKE_START) == VOICE_PTT_NONE);
    assert(voice_input_step(&ptt, &wake, true, false, false, false, false, VOICE_WAKE_START) == VOICE_PTT_START && wake);
    /* Releasing/never pressing the button must not end a voice-started turn. */
    for (int i = 0; i < 20; ++i)
        assert(voice_input_step(&ptt, &wake, true, false, true, false, false, VOICE_WAKE_START) == VOICE_PTT_NONE && wake);
    assert(voice_input_step(&ptt, &wake, true, false, true, false, false, VOICE_WAKE_END) == VOICE_PTT_END);
    /* PTT takeover reuses the same turn; only release emits end. */
    assert(voice_input_step(&ptt, &wake, true, false, true, true, false, VOICE_WAKE_NONE) == VOICE_PTT_NONE && wake);
    assert(voice_input_step(&ptt, &wake, true, false, true, true, false, VOICE_WAKE_END) == VOICE_PTT_NONE && !wake);
    assert(voice_input_step(&ptt, &wake, true, false, true, false, false, VOICE_WAKE_END) == VOICE_PTT_NONE);
    assert(voice_input_step(&ptt, &wake, true, false, true, false, false, VOICE_WAKE_NONE) == VOICE_PTT_END);
    /* PTT wins over simultaneous wake, no second start or VAD end. */
    assert(voice_input_step(&ptt, &wake, true, false, false, true, false, VOICE_WAKE_START) == VOICE_PTT_NONE);
    assert(voice_input_step(&ptt, &wake, true, false, false, true, false, VOICE_WAKE_START) == VOICE_PTT_START && !wake);
    assert(voice_input_step(&ptt, &wake, true, false, true, true, false, VOICE_WAKE_END) == VOICE_PTT_NONE);
    assert(voice_input_step(&ptt, &wake, false, false, false, true, false, VOICE_WAKE_START) == VOICE_PTT_NONE && !wake);

    /* Idle has noise estimation only, never keyword detection. Playback with
     * no button has no speech-onset detector even when AEC is ready. */
    assert(voice_listen_mode(false,false,false,false,false,false,true) == VOICE_WAKE_NOISE);
    assert(voice_listen_mode(false,false,true,true,false,false,true) == VOICE_WAKE_OFF);
    assert(voice_listen_mode(false,false,true,true,true,true,false) == VOICE_WAKE_OFF);
    assert(voice_listen_mode(false,false,true,true,true,true,true) == VOICE_WAKE_BARGE);
    assert(voice_listen_mode(false,false,true,false,false,false,true) == VOICE_WAKE_FOLLOWUP);
    assert(voice_listen_mode(true,true,true,false,false,false,true) == VOICE_WAKE_CAPTURE);
    ptt = (voice_ptt_t){0}; wake = false;
    /* A bystander or stale detection without a button cannot interrupt. */
    assert(voice_input_step(&ptt,&wake,true,false,false,false,true,VOICE_WAKE_START) == VOICE_PTT_NONE);
    /* Silent hold does not start, including after the old reply finishes. */
    for (int i=0; i<100; ++i)
        assert(voice_input_step(&ptt,&wake,true,false,false,true,true,VOICE_WAKE_NONE) == VOICE_PTT_NONE);
    assert(ptt.speech_gated);
    for (int i=0; i<100; ++i)
        assert(voice_input_step(&ptt,&wake,true,false,false,true,false,VOICE_WAKE_NONE) == VOICE_PTT_NONE);
    assert(voice_input_step(&ptt,&wake,true,false,false,false,false,VOICE_WAKE_START) == VOICE_PTT_NONE);
    assert(!ptt.speech_gated);
    /* A fresh ordinary idle press starts a first turn; release ends it. */
    assert(voice_input_step(&ptt,&wake,true,false,false,false,false,VOICE_WAKE_NONE) == VOICE_PTT_NONE);
    assert(voice_input_step(&ptt,&wake,true,false,false,true,false,VOICE_WAKE_NONE) == VOICE_PTT_NONE);
    assert(voice_input_step(&ptt,&wake,true,false,false,true,false,VOICE_WAKE_NONE) == VOICE_PTT_START);
    assert(voice_input_step(&ptt,&wake,true,false,true,false,false,VOICE_WAKE_NONE) == VOICE_PTT_NONE);
    assert(voice_input_step(&ptt,&wake,true,false,true,false,false,VOICE_WAKE_NONE) == VOICE_PTT_END);
    /* Busy press + qualified speech emits exactly one start and is PTT-owned. */
    assert(voice_input_step(&ptt,&wake,true,false,false,true,true,VOICE_WAKE_NONE) == VOICE_PTT_NONE);
    assert(voice_input_step(&ptt,&wake,true,false,false,true,true,VOICE_WAKE_START) == VOICE_PTT_START && !wake);
    for (int i=0; i<20; ++i)
        assert(voice_input_step(&ptt,&wake,true,false,true,true,false,VOICE_WAKE_START) == VOICE_PTT_NONE);
    assert(voice_input_step(&ptt,&wake,true,false,true,false,false,VOICE_WAKE_NONE) == VOICE_PTT_NONE);
    assert(voice_input_step(&ptt,&wake,true,false,true,false,false,VOICE_WAKE_NONE) == VOICE_PTT_END);
    /* Stop/negotiation barriers and disconnect reject even qualified speech. */
    assert(voice_input_step(&ptt,&wake,true,true,false,true,true,VOICE_WAKE_START) == VOICE_PTT_NONE);
    assert(voice_input_step(&ptt,&wake,false,false,false,true,true,VOICE_WAKE_START) == VOICE_PTT_NONE);
    assert(!ptt.speech_gated);
    puts("voice input: button+speech gating, silent hold/release, follow-up, endpoint and offline barriers passed");
    return 0;
}
