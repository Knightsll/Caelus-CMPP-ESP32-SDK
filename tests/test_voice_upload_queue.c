/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_upload_queue.h"
#include "voice_lifecycle.h"
#include "voice_diagnostics.h"
#include <assert.h>
#include <stdio.h>

static voice_upload_queue_t q;
static int scheduled, schedule_error;
static int submit(void *context)
{
    assert(context == &q);
    ++scheduled;
    return schedule_error;
}
static size_t push(bool text, int id, uint32_t generation)
{
    voice_packet_t p = {.text = text, .bytes = text ? 4 : 640, .generation = generation};
    p.pcm[0] = (int16_t)id;
    size_t dropped = 999;
    assert(voice_upload_queue_push(&q, &p, &dropped));
    return dropped;
}
static void expect(bool text, int id, uint32_t generation)
{
    voice_packet_t p;
    assert(voice_upload_queue_pop(&q, &p));
    assert(p.text == text && p.pcm[0] == id && p.generation == generation);
}
static void completed_work(void)
{
    q.work_pending = false;
    assert(voice_upload_queue_schedule(&q, submit, &q) == 0);
}
int main(void)
{
    /* 40ms, 3s and 15s turns with a 200ms/754ms/1.5s sender pause.
     * This is a sample-clock simulation using the production queue, not a
     * wall-clock/RTOS throughput test. Retained PCM and end must stay ordered. */
    const int durations[] = {2, 150, 750};
    const int pauses[] = {10, 38, 75};
    for (size_t run = 0; run < 3; ++run) {
        voice_upload_queue_reset(&q);
        assert(push(true, -1, 1) == 0);
        int next = 0;
        expect(true, -1, 1);
        for (int frame = 0; frame < durations[run]; ++frame) {
            assert(push(false, frame, 1) == 0);
            if (frame >= pauses[run]) expect(false, next++, 1);
        }
        assert(push(true, -2, 1) == 0);
        while (next < durations[run]) expect(false, next++, 1);
        expect(true, -2, 1);
        assert(!q.count);
    }
    /* Congestion: keep start/end even when PCM is full. Oldest PCM alone
     * is discarded, including when a control item is at the FIFO head. */
    assert(push(true, -1, 7) == 0);
    for (int i = 0; i < VOICE_UPLOAD_PCM_FRAMES; ++i) assert(push(false, i, 7) == 0);
    assert(q.count == VOICE_UPLOAD_PCM_FRAMES + 1);
    size_t lost = 0;
    for (int i = VOICE_UPLOAD_PCM_FRAMES; i < VOICE_UPLOAD_PCM_FRAMES + 50; ++i)
        lost += push(false, i, 7);
    assert(lost == 50 * 640);
    assert(push(true, -2, 7) == 0);
    assert(q.count == VOICE_UPLOAD_PCM_FRAMES + 2);
    expect(true, -1, 7);
    for (int i = 50; i < VOICE_UPLOAD_PCM_FRAMES + 50; ++i) expect(false, i, 7);
    expect(true, -2, 7);

    /* All four control slots are reserved; a fifth is an explicit separate
     * failure, never eviction. Audio can fill the other 120 slots. */
    for (int i = 0; i < VOICE_UPLOAD_CONTROL_SLOTS; ++i) assert(push(true, i, 8) == 0);
    for (int i = 0; i < VOICE_UPLOAD_PCM_FRAMES; ++i) assert(push(false, i, 8) == 0);
    assert(q.count == VOICE_UPLOAD_SLOTS);
    voice_packet_t extra = {.text = true, .bytes = 4};
    size_t dropped;
    assert(!voice_upload_queue_push(&q, &extra, &dropped) && !dropped);
    assert(q.count == VOICE_UPLOAD_SLOTS);
    voice_diagnostics_t diagnostic = {0};
    voice_diag_upload_failure(&diagnostic, VOICE_STOP_UPLOAD_CONTROL_FULL, "input.end", 8, 0, 0);
    assert(diagnostic.upload_control_queue_full_count == 1 && !diagnostic.upload_queue_full_count);
    for (int i = 0; i < VOICE_UPLOAD_CONTROL_SLOTS; ++i) expect(true, i, 8);
    voice_upload_queue_reset(&q);

    /* Pending callback survives reset, but its old audio does not. New pushes
     * cannot schedule a second writer. The outstanding callback sends only the
     * new generation, then schedules exactly one successor. */
    voice_lifecycle_t life = {0};
    voice_lifecycle_open(&life);
    uint32_t old = life.generation;
    push(true, -1, old);
    assert(!voice_upload_queue_schedule(&q, submit, &q) && scheduled == 1);
    voice_lifecycle_stop(&life, VOICE_STOP_PEER_CLOSED);
    voice_upload_queue_reset(&q);
    assert(q.work_pending && !q.count);
    voice_lifecycle_open(&life);
    push(true, -1, life.generation);
    push(false, 42, life.generation);
    assert(!voice_upload_queue_schedule(&q, submit, &q) && scheduled == 1);
    expect(true, -1, life.generation);
    completed_work();
    assert(scheduled == 2 && q.work_pending);
    expect(false, 42, life.generation);
    completed_work();
    assert(!q.work_pending && scheduled == 2);

    /* Reset while a send is in flight: late send failure cannot kill the new
     * epoch; queued new audio starts only after the old work ticket returns. */
    push(false, 1, life.generation);
    assert(!voice_upload_queue_schedule(&q, submit, &q));
    voice_packet_t in_flight;
    assert(voice_upload_queue_pop(&q, &in_flight));
    voice_lifecycle_stop(&life, VOICE_STOP_REQUESTED);
    voice_upload_queue_reset(&q);
    voice_lifecycle_open(&life);
    push(false, 2, life.generation);
    assert(!voice_lifecycle_accept(&life, in_flight.generation));
    assert(q.work_pending);
    completed_work();
    assert(q.work_pending);
    expect(false, 2, life.generation);
    completed_work();

    /* Real socket failure cleans up queued end/PCM; scheduler failure keeps
     * its factual error and releases the ticket so cleanup/retry is possible. */
    push(false, 3, life.generation);
    push(true, -2, life.generation);
    assert(voice_lifecycle_stop(&life, VOICE_STOP_SEND_FAILED));
    voice_upload_queue_reset(&q);
    assert(!q.count && life.fault && life.last_stop_reason == VOICE_STOP_SEND_FAILED);
    schedule_error = 17;
    push(false, 4, 99);
    assert(voice_upload_queue_schedule(&q, submit, &q) == 17);
    assert(!q.work_pending && q.count == 1);
    schedule_error = 0;
    assert(!voice_upload_queue_schedule(&q, submit, &q) && q.work_pending);
    voice_upload_queue_reset(&q);
    completed_work();
    assert(!q.work_pending && !q.count);
    printf("upload queue: ordered controls, bounded PCM loss, callback/reset isolation; %zu host bytes\n", sizeof(q));
    return 0;
}
