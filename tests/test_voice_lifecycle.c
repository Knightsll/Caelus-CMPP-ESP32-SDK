/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "voice_lifecycle.h"
#include "voice_status.h"
#include "voice_diagnostics.h"

int main(void)
{
    voice_lifecycle_t s = {0};
    assert(!voice_lifecycle_accept(&s, 0));
    assert(!voice_lifecycle_stop(&s, VOICE_STOP_PEER_CLOSED));
    assert(s.last_stop_reason == VOICE_STOP_NONE && !s.fault);

    /* HTTP starts reading, MCP closes, then the late frame/send callback runs. */
    voice_lifecycle_open(&s);
    uint32_t reading_generation = s.generation;
    assert(voice_lifecycle_accept(&s, reading_generation));
    assert(voice_lifecycle_stop(&s, VOICE_STOP_REQUESTED));
    assert(!voice_lifecycle_accept(&s, reading_generation));
    assert(!voice_lifecycle_stop(&s, VOICE_STOP_INVALID_FRAME));
    assert(!voice_lifecycle_stop(&s, VOICE_STOP_PEER_CLOSED));
    assert(s.last_stop_reason == VOICE_STOP_REQUESTED && !s.fault);
    assert(s.last_fault_reason == VOICE_STOP_NONE);
    assert(!voice_status_lit(VOICE_LED_NETWORK, 50));

    voice_lifecycle_open(&s);
    assert(!voice_lifecycle_accept(&s, reading_generation));
    assert(voice_lifecycle_accept(&s, s.generation));
    voice_lifecycle_ready(&s);
    assert(!s.fault);

    /* Every ordinary stop is recorded, without manufacturing a fault. */
    for (int reason = VOICE_STOP_REQUESTED; reason <= VOICE_STOP_OPEN_TIMEOUT; ++reason) {
        if (!s.active) voice_lifecycle_open(&s);
        assert(voice_lifecycle_stop(&s, (voice_stop_reason_t)reason));
        assert(s.last_stop_reason == (voice_stop_reason_t)reason && !s.fault);
        assert(strcmp(voice_stop_reason_name(s.last_stop_reason), "unknown") != 0);
    }

    /* Real faults survive callbacks, ordinary cleanup, and an unverified open.
     * A new hello clears the LED but retains the last factual error for diagnosis. */
    for (int reason = VOICE_STOP_SEND_FAILED; reason <= VOICE_STOP_UPLOAD_CONTROL_FULL; ++reason) {
        voice_lifecycle_open(&s);
        voice_lifecycle_ready(&s);
        assert(voice_lifecycle_stop(&s, (voice_stop_reason_t)reason));
        assert(s.fault && s.last_fault_reason == (voice_stop_reason_t)reason);
        assert(voice_status_lit(VOICE_LED_NETWORK | VOICE_LED_FAULT, 50));
        assert(!voice_lifecycle_stop(&s, VOICE_STOP_PEER_CLOSED));
        assert(s.last_stop_reason == (voice_stop_reason_t)reason && s.fault);
        voice_lifecycle_ready(&s); /* Closed-session hello must not clear fault. */
        assert(s.fault);
        voice_lifecycle_open(&s);
        assert(s.fault);
        assert(voice_lifecycle_stop(&s, VOICE_STOP_OPEN_TIMEOUT));
        assert(s.fault && s.last_fault_reason == (voice_stop_reason_t)reason);
        voice_lifecycle_open(&s);
        voice_lifecycle_ready(&s);
        assert(!s.fault && s.last_fault_reason == (voice_stop_reason_t)reason);
        assert(strcmp(voice_stop_reason_name(s.last_fault_reason), "unknown") != 0);
        assert(voice_lifecycle_stop(&s, VOICE_STOP_REQUESTED));
    }

    voice_lifecycle_open(&s);
    assert(voice_lifecycle_stop(&s, VOICE_STOP_REQUESTED));
    voice_lifecycle_audio_stop_failed(&s);
    assert(s.fault && s.last_fault_reason == VOICE_STOP_AUDIO_STOP_FAILED);
    assert(s.last_stop_reason == VOICE_STOP_REQUESTED); /* Intent != cleanup success. */
    assert(!voice_lifecycle_stop(&s, VOICE_STOP_PEER_CLOSED));
    assert(s.fault && s.last_fault_reason == VOICE_STOP_AUDIO_STOP_FAILED);
    /* Queue pressure and scheduler failure remain distinct across reconnects. */
    voice_diagnostics_t d = {0};
    assert(voice_diag_upload_failure(&d, VOICE_STOP_UPLOAD_QUEUE_FULL, "pcm", 7, 0, 0)
           == VOICE_STOP_UPLOAD_QUEUE_FULL);
    assert(d.upload_queue_full_count == 1 && d.upload_work_schedule_failed_count == 0);
    voice_diag_upload_failure(&d, VOICE_STOP_UPLOAD_WORK_FAILED, "input.end", 8, -1, 12);
    assert(d.upload_queue_full_count == 1 && d.upload_work_schedule_failed_count == 1);
    assert(d.last_upload_schedule_error == -1 && d.last_upload_schedule_errno == 12);
    assert(d.last_upload_failure_generation == 8 && !strcmp(d.last_upload_failure_packet, "input.end"));
    voice_diag_send(&d, 210000, 640, 0);
    voice_diag_send(&d, 100, 640, 1);
    assert(d.upload_pcm_sent_bytes == 640 && d.upload_send_failed_count == 1);
    assert(d.upload_send_max_us == 210000);
    voice_lifecycle_open(&s);
    voice_lifecycle_ready(&s);
    assert(d.upload_queue_full_count == 1 && d.last_upload_failure_generation == 8);
    d.upload_pcm_sent_bytes = UINT32_MAX - 100;
    voice_diag_send(&d, (uint64_t)UINT32_MAX + 1, 640, 0);
    assert(d.upload_pcm_sent_bytes == UINT32_MAX && d.upload_send_max_us == UINT32_MAX);
    voice_diag_add(&d.upload_pcm_sent_bytes, 1);
    assert(d.upload_pcm_sent_bytes == UINT32_MAX);
    puts("voice lifecycle: late frames, stale callbacks, idempotent close and factual diagnostics passed");
    return 0;
}
