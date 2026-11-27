/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_wake_logic.h"

bool voice_conversation_step(voice_conversation_t *s, bool ready, bool busy,
                             bool voice_started, int64_t now, unsigned timeout_ms)
{
    if (!ready) { *s = (voice_conversation_t){0}; return false; }
    if (voice_started) s->active = true;
    if (s->active && (busy || voice_started)) s->deadline_ms = now + timeout_ms;
    if (s->active && now >= s->deadline_ms) s->active = false;
    return s->active;
}
