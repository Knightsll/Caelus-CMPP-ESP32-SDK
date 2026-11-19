/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_upload_queue.h"
#include <string.h>

void voice_upload_queue_reset(voice_upload_queue_t *q)
{
    /* A queued callback still owns its ticket after connection teardown. */
    bool pending = q->work_pending;
    memset(q, 0, sizeof(*q));
    q->work_pending = pending;
}

static void remove_at(voice_upload_queue_t *q, size_t position)
{
    size_t slot = q->order[position];
    if (!q->packets[slot].text) --q->pcm_count;
    q->used[slot] = false;
    --q->count;
    memmove(q->order + position, q->order + position + 1,
            (q->count - position) * sizeof(q->order[0]));
}

bool voice_upload_queue_push(voice_upload_queue_t *q, const voice_packet_t *packet,
                             size_t *dropped_bytes)
{
    *dropped_bytes = 0;
    if (packet->bytes == 0 || packet->bytes > VOICE_UPLOAD_PACKET_BYTES ||
        (!packet->text && packet->bytes % sizeof(int16_t))) return false;
    if (packet->text) {
        if (q->count - q->pcm_count == VOICE_UPLOAD_CONTROL_SLOTS) return false;
    } else if (q->pcm_count == VOICE_UPLOAD_PCM_FRAMES) {
        for (size_t i = 0; i < q->count; ++i) {
            if (!q->packets[q->order[i]].text) {
                *dropped_bytes = q->packets[q->order[i]].bytes;
                remove_at(q, i);
                break;
            }
        }
    }
    for (size_t slot = 0; slot < VOICE_UPLOAD_SLOTS; ++slot) {
        if (!q->used[slot]) {
            q->packets[slot] = *packet;
            q->used[slot] = true;
            q->order[q->count++] = (uint16_t)slot;
            if (!packet->text) ++q->pcm_count;
            return true;
        }
    }
    return false;
}

bool voice_upload_queue_pop(voice_upload_queue_t *q, voice_packet_t *packet)
{
    if (!q->count) return false;
    *packet = q->packets[q->order[0]];
    remove_at(q, 0);
    return true;
}

int voice_upload_queue_schedule(voice_upload_queue_t *q, int (*submit)(void *), void *arg)
{
    if (q->work_pending || !q->count) return 0;
    q->work_pending = true;
    int err = submit(arg);
    if (err) q->work_pending = false;
    return err;
}
