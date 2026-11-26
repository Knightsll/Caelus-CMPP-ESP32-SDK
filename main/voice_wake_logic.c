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

bool voice_followup_step(voice_onset_t *s, bool speech, double power, double noise_power,
                         bool barge, unsigned barge_min_rms)
{
    /* Keep the energy/VAD floor, but allow syllable gaps: 200ms qualified
     * speech within 400ms. A single weak consonant must not erase an onset.
     * Playback barge-in retains its stricter contiguous 300ms requirement. */
    double minimum = barge ? (double)barge_min_rms * barge_min_rms : 6400;
    bool qualified = speech && power >= minimum && power >= noise_power * 4;
    s->history = ((s->history << 1) | qualified) & 0xfffff;
    s->consecutive_ms = qualified ? s->consecutive_ms + 20 : 0;
    unsigned frames = 0;
    for (uint32_t bits = s->history; bits; bits >>= 1) frames += bits & 1;
    return qualified && (barge ? s->consecutive_ms >= 300 : frames >= 10);
}

bool voice_endpoint_step(voice_endpoint_t *s, bool speech, unsigned silence_ms)
{
    s->elapsed_ms += 20;
    if (speech) { s->speech_ms += 20; s->silence_ms = 0; }
    else s->silence_ms += 20;
    return s->elapsed_ms >= 15000 ||
        (s->speech_ms >= 100 && s->silence_ms >= silence_ms) ||
        (s->speech_ms < 100 && s->elapsed_ms >= 5000);
}

voice_ptt_event_t voice_input_step(voice_ptt_t *ptt, bool *wake_recording,
                                 bool ready, bool blocked, bool recording,
                                 bool pressed, bool reply_busy, voice_wake_event_t wake)
{
    if (!recording) *wake_recording = false;
    if (!ready) ptt->speech_gated = false;
    /* A press made during a reply remains speech-gated until release, even
     * when playback ends meanwhile. Never turn a silent hold into a new turn. */
    if (!recording && (reply_busy || ptt->speech_gated)) {
        ptt->speech_gated = ready && pressed;
        voice_ptt_step(ptt, ready, true, false, pressed);
        if (ready && !blocked && pressed && wake == VOICE_WAKE_START) {
            ptt->speech_gated = false;
            return VOICE_PTT_START; /* PTT release ends this turn. */
        }
        return VOICE_PTT_NONE;
    }

    voice_ptt_event_t event = voice_ptt_step(ptt, ready, blocked,
                                            recording && !*wake_recording, pressed);
    if (!ready || blocked) return VOICE_PTT_NONE;
    if (event == VOICE_PTT_START && recording && *wake_recording) {
        *wake_recording = false;
        return VOICE_PTT_NONE;
    }
    if (event != VOICE_PTT_NONE) return event;
    if (!recording && !pressed && wake == VOICE_WAKE_START) {
        *wake_recording = true;
        return VOICE_PTT_START;
    }
    if (recording && *wake_recording && wake == VOICE_WAKE_END) return VOICE_PTT_END;
    return VOICE_PTT_NONE;
}

voice_wake_mode_t voice_listen_mode(bool recording, bool automatic, bool followup,
    bool reply_busy, bool gated, bool pressed, bool echo_ready)
{
    if (recording) return automatic ? VOICE_WAKE_CAPTURE : VOICE_WAKE_OFF;
    if (gated) return pressed && (!reply_busy || echo_ready) ? VOICE_WAKE_BARGE : VOICE_WAKE_OFF;
    if (reply_busy || pressed) return VOICE_WAKE_OFF;
    return followup ? VOICE_WAKE_FOLLOWUP : VOICE_WAKE_NOISE;
}
