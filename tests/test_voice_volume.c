/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "voice_volume.h"
#include "nvs.h"

static int live, saved = -1, pending, sets, commits, fail_codec, fail_open, fail_write, fail_commit;
static bool muted, ignore_mute_write;
static int reads, fail_read_reg = -1;
static int device;
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t h, int volume)
{
    assert(h == &device);
    if (fail_codec) return -1;
    live = volume;
    ++sets;
    return 0;
}
int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t h, bool mute)
{
    assert(h == &device);
    if (!ignore_mute_write) muted = mute;
    return 0;
}
int esp_codec_dev_read_reg(esp_codec_dev_handle_t h, int reg, int *value)
{
    assert(h == &device);
    ++reads;
    if (reg == fail_read_reg) { *value = 0; return -10; }
    *value = reg == 0x31 ? (muted ? 0x60 : 0) : reg == 0x32 ? live : 0x80;
    return 0;
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h)
{
    assert(!strcmp(name, "mcp_light"));
    if (fail_open) return -7;
    if (mode == NVS_READONLY && saved < 0) return ESP_ERR_NVS_NOT_FOUND;
    *h = 1;
    return 0;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *value)
{
    assert(h == 1 && !strcmp(key, "voice_volume"));
    if (saved < 0) return ESP_ERR_NVS_NOT_FOUND;
    *value = (uint8_t)saved;
    return 0;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{
    assert(h == 1 && !strcmp(key, "voice_volume"));
    if (fail_write) return -8;
    pending = value;
    return 0;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    assert(h == 1);
    ++commits;
    if (fail_commit) return -9;
    saved = pending;
    return 0;
}
void nvs_close(nvs_handle_t h) { assert(h == 1); }

int main(void)
{
    bool changed = true;
    assert(voice_volume_set(50, &changed) == ESP_ERR_INVALID_STATE && !changed);
    assert(voice_volume_init(&device) == 0);
    assert(live == 35 && voice_volume_get() == 35 && muted && saved == -1);
    const voice_codec_readback_t *d = voice_volume_readback();
    assert(d->read_count == 18 && d->read_failed_count == 0 && d->unmute_read_count == 0);
    assert(d->boot_values[13] == 0x60 && d->boot_values[14] == 35);
    assert(d->boot_errors[13] == 0 && d->volume_reg32 == 35);
    int read_before = reads;
    (void)voice_volume_readback();
    (void)voice_volume_get();
    assert(reads == read_before); /* State uses a snapshot, never bus polling. */
    int before = sets;
    assert(voice_volume_set(-1, &changed) == ESP_ERR_INVALID_ARG && !changed);
    assert(voice_volume_set(101, &changed) == ESP_ERR_INVALID_ARG && !changed);
    assert(sets == before);
    assert(voice_volume_set(100, &changed) == 0 && changed);
    assert(live == 100 && saved == 100 && muted); /* Idle stays muted. */
    before = commits;
    assert(voice_volume_set(100, &changed) == 0 && !changed && commits == before);
    assert(voice_volume_mute(false) == 0 && !muted);
    assert(voice_volume_set(0, &changed) == 0 && muted && saved == 0);
    assert(voice_volume_mute(false) == 0 && muted); /* PTT/output cannot override zero. */
    assert(voice_volume_set(80, &changed) == 0 && !muted && live == 80);
    assert(voice_volume_mute(true) == 0 && muted);
    assert(voice_volume_init(&device) == 0 && live == 80 && muted); /* Boot restores config. */
    fail_codec = 1;
    assert(voice_volume_set(20, &changed) == ESP_FAIL && !changed && saved == 80);
    fail_codec = 0;
    for (int failure = 0; failure < 3; ++failure) {
        fail_open = failure == 0; fail_write = failure == 1; fail_commit = failure == 2;
        int requested = 40 + failure;
        assert(voice_volume_set(requested, &changed) != 0 && changed);
        assert(live == requested && voice_volume_get() == requested && saved != requested);
        fail_open = fail_write = fail_commit = 0;
        assert(voice_volume_set(requested, &changed) == 0 && changed && saved == requested);
    }
    /* A masked mute write is exposed by real readback, not the API cache. */
    assert(voice_volume_mute(true) == 0 && muted);
    ignore_mute_write = true;
    assert(voice_volume_mute(false) == 0);
    assert(!d->requested_mute && d->last_unmute_reg31 == 0x60 && d->last_unmute_error == 0);
    ignore_mute_write = false;
    fail_read_reg = 0x31;
    assert(voice_volume_mute(false) == 0); /* Read failure never changes control return. */
    assert(d->last_unmute_reg31 == -1 && d->last_unmute_error == -10 && d->read_failed_count == 1);
    fail_read_reg = 0x02;
    assert(voice_volume_init(&device) == 0);
    assert(d->boot_values[2] == -1 && d->boot_errors[2] == -10);
    assert(d->boot_values[3] == 0x80 && d->boot_errors[3] == 0);
    fail_read_reg = -1;
    saved = 255;
    assert(voice_volume_init(&device) == ESP_ERR_INVALID_ARG);
    saved = -1; /* Factory erase removes the shared mcp_light namespace. */
    assert(voice_volume_init(&device) == 0 && live == 35 && muted);
    puts("voice volume: live changes, zero mute, boot restore, boundaries and persistence retry passed");
    return 0;
}
