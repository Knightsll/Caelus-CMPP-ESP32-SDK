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

    /* Every turn is controlled by SW2. Silence/loudness have no input here. */
    voice_ptt_t ptt = {0};
    for (int i=0; i<100; ++i)
        assert(voice_ptt_step(&ptt,true,false,false,false)==VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt,true,false,false,true)==VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt,true,false,false,true)==VOICE_PTT_START);
    for (int i=0; i<100; ++i)
        assert(voice_ptt_step(&ptt,true,false,true,true)==VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt,true,false,true,false)==VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt,true,false,true,false)==VOICE_PTT_END);
    /* A second press during a negotiated reply immediately starts another turn. */
    assert(voice_ptt_step(&ptt,true,false,false,true)==VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt,true,false,false,true)==VOICE_PTT_START);
    /* Stop/offline/non-negotiated playback disarm; require release/repress. */
    assert(voice_ptt_step(&ptt,true,true,false,true)==VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt,true,false,false,true)==VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt,false,false,false,true)==VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt,true,false,false,false)==VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt,true,false,false,true)==VOICE_PTT_NONE);
    assert(voice_ptt_step(&ptt,true,false,false,true)==VOICE_PTT_START);
    puts("voice PTT: press/start, hold, release/end, interruption and idle timeout passed");
    return 0;
}
