/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "voice_board.h"
#include "voice_media.h"

static cmpp_capability_ops_t ops;
static void *context;
static const cmpp_capability_descriptor_t *descriptor;
static int media_token, calls, failure;
static voice_media_state_t current = {.volume = 35, .last_fault_reason = "none", .last_stop_reason = "none"};
cmpp_status_t cmpp_runtime_register_capability(cmpp_runtime_t *r, const cmpp_capability_descriptor_t *d,
                                               const cmpp_capability_ops_t *o, void *c)
{ (void)r; descriptor = d; ops = *o; context = c; return CMPP_OK; }
cmpp_status_t cmpp_runtime_unregister_capability(cmpp_runtime_t *r, const char *id)
{ (void)r; (void)id; return CMPP_OK; }
const cmpp_value_t *cmpp_value_object_get(const cmpp_value_t *o, const char *key)
{
    if (!o || o->type != CMPP_VALUE_OBJECT) return NULL;
    for (size_t i = 0; i < o->as.object.count; ++i)
        if (!strcmp(o->as.object.entries[i].key, key)) return o->as.object.entries[i].value;
    return NULL;
}
esp_err_t voice_media_create(voice_media_t **m) { *m = (voice_media_t *)&media_token; return ESP_OK; }
esp_err_t voice_media_start(voice_media_t *m, const device_identity_t *id)
{ (void)m; (void)id; return ESP_OK; }
esp_err_t voice_media_open(voice_media_t *m, bool dry)
{ (void)m; (void)dry; return ESP_OK; }
esp_err_t voice_media_close(voice_media_t *m, const char *id, bool dry, bool *changed)
{ (void)m; (void)id; *changed = !dry; return ESP_OK; }
esp_err_t voice_media_stop(voice_media_t *m) { (void)m; return ESP_OK; }
void voice_media_observe(voice_media_t *m, voice_media_state_t *s) { (void)m; *s = current; }
esp_err_t voice_media_set_volume(voice_media_t *m, int volume, bool dry, bool *changed)
{
    (void)m;
    *changed = !dry && volume != current.volume;
    if (!dry) { ++calls; current.volume = volume; }
    return failure;
}

int main(void)
{
    board_runtime_t *b;
    assert(board_runtime_create((cmpp_runtime_t *)&media_token, &b) == ESP_OK);
    assert(descriptor->command_type_count == 3 && descriptor->supports_dry_run);
    cmpp_value_t volume = {.type = CMPP_VALUE_INT, .as.integer = 100};
    cmpp_object_entry_t entry = {.key = "volume", .value = &volume};
    cmpp_value_t args = {.type = CMPP_VALUE_OBJECT, .as.object = {.entries = &entry, .count = 1}};
    cmpp_request_t request = {.command_type = "voice.channel.volume.set", .arguments = &args, .dry_run = true};
    assert(ops.validate(context, &args) == CMPP_OK && ops.admit(context, &request) == CMPP_OK);
    cmpp_result_t result = {0};
    assert(ops.execute(context, &request, &result) == CMPP_OK);
    assert(calls == 0 && result.effect == CMPP_EFFECT_NO_EFFECT);
    assert(cmpp_value_object_get(result.data, "volume")->as.integer == 35);
    request.dry_run = false;
    current.diagnostics.upload_pcm_dropped_frames = 7;
    current.diagnostics.upload_pcm_dropped_bytes = 4480;
    current.diagnostics.upload_control_queue_full_count = 2;
    assert(ops.execute(context, &request, &result) == CMPP_OK && calls == 1);
    assert(result.outcome == CMPP_OUTCOME_COMPLETED && result.effect == CMPP_EFFECT_COMMITTED);
    assert(cmpp_value_object_get(result.data, "volume")->as.integer == 100);
    assert(result.data->as.object.count == 15);
    assert(!cmpp_value_object_get(result.data, "diagnostics"));
    assert(!cmpp_value_object_get(result.data, "codec_readback"));
    cmpp_value_t state;
    assert(ops.observe(context, &state) == CMPP_OK);
    assert(state.as.object.count == 17);
    const cmpp_value_t *diagnostics = cmpp_value_object_get(&state, "diagnostics");
    assert(diagnostics->as.object.count == 19);
    assert(cmpp_value_object_get(diagnostics, "upload_pcm_dropped_frames")->as.integer == 7);
    assert(cmpp_value_object_get(diagnostics, "upload_pcm_dropped_bytes")->as.integer == 4480);
    assert(cmpp_value_object_get(diagnostics, "upload_control_queue_full_count")->as.integer == 2);
    const cmpp_value_t *codec = cmpp_value_object_get(&state, "codec_readback");
    assert(codec && codec->as.object.count == 13);
    assert(cmpp_value_object_get(codec, "boot_values")->as.object.count == 16);
    assert(cmpp_value_object_get(codec, "boot_errors")->as.object.count == 16);
    volume.as.integer = 0;
    assert(ops.validate(context, &args) == CMPP_OK);
    assert(ops.execute(context, &request, &result) == CMPP_OK);
    assert(cmpp_value_object_get(result.data, "volume")->as.integer == 0);
    volume.as.integer = 101; assert(ops.validate(context, &args) != CMPP_OK);
    volume.as.integer = -1; assert(ops.validate(context, &args) != CMPP_OK);
    volume.type = CMPP_VALUE_BOOL; assert(ops.validate(context, &args) != CMPP_OK);
    volume.type = CMPP_VALUE_DOUBLE; assert(ops.validate(context, &args) != CMPP_OK);
    volume.type = CMPP_VALUE_INT; volume.as.integer = 35;
    request.command_type = "voice.channel.open"; assert(ops.admit(context, &request) != CMPP_OK);
    request.command_type = "voice.channel.close"; assert(ops.admit(context, &request) != CMPP_OK);
    args.as.object.count = 0;
    assert(ops.validate(context, &args) == CMPP_OK);
    request.command_type = "voice.channel.volume.set"; assert(ops.admit(context, &request) != CMPP_OK);
    request.command_type = "voice.channel.open"; assert(ops.admit(context, &request) == CMPP_OK);
    strcpy(current.channel_id, "0123456789abcdef0123456789abcdef");
    assert(ops.execute(context, &request, &result) == CMPP_OK);
    assert(result.data->as.object.count == 15);
    assert(!cmpp_value_object_get(result.data, "diagnostics"));
    assert(!strcmp(cmpp_value_object_get(result.data, "channel_id")->as.string, current.channel_id));
    assert(!strcmp(cmpp_value_object_get(result.data, "ws_path")->as.string, "/voice"));
    assert(cmpp_value_object_get(result.data, "sample_rate")->as.integer == 16000);
    args.as.object.count = 1; entry.key = "channel_id";
    volume.type = CMPP_VALUE_STRING; volume.as.string = "channel_1";
    assert(ops.validate(context, &args) == CMPP_OK);
    request.command_type = "voice.channel.close"; assert(ops.admit(context, &request) == CMPP_OK);
    assert(ops.execute(context, &request, &result) == CMPP_OK);
    assert(!cmpp_value_object_get(result.data, "codec_readback"));
    request.command_type = "voice.channel.volume.set"; assert(ops.admit(context, &request) != CMPP_OK);
    entry.key = "volume"; volume.type = CMPP_VALUE_INT; volume.as.integer = 50;
    failure = ESP_FAIL;
    assert(ops.execute(context, &request, &result) == CMPP_ERR_IO);
    assert(result.outcome == CMPP_OUTCOME_FAILED && result.effect == CMPP_EFFECT_UNKNOWN);
    assert(!strcmp(result.error_code, "volume_set_failed"));
    assert(cmpp_value_object_get(result.data, "volume")->as.integer == 50);
    board_runtime_destroy(b);
    puts("voice board: command shape, dry-run, result volume, preserved state and partial failure passed");
    return 0;
}
