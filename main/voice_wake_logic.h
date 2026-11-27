/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "voice_wire.h"

/* Historical filename: only the local session idle timeout remains. */
typedef struct { bool active; int64_t deadline_ms; } voice_conversation_t;
/* Quiet timeout ends local follow-up listening, never the media connection.
 * Recording/thinking/playback suspend the timeout until the user can speak. */
bool voice_conversation_step(voice_conversation_t *state, bool ready, bool busy,
                             bool voice_started, int64_t now_ms, unsigned timeout_ms);
