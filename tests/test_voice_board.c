/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "voice_board.h"
#include "voice_media.h"

static cmpp_capability_ops_t ops, threshold_ops;
static void *context;
static const cmpp_capability_descriptor_t *descriptor, *threshold_descriptor;
static int media_token, calls, failure;
static voice_media_state_t current = {.volume = 35, .barge_min_rms = 1000, .last_fault_reason = "none", .last_stop_reason = "none",
    .wake_status = "model_not_configured", .wake_word = "小白小白"};
cmpp_status_t cmpp_runtime_register_capability(cmpp_runtime_t *r, const cmpp_capability_descriptor_t *d,
                                               const cmpp_capability_ops_t *o, void *c)
{ (void)r; context = c; if (!strcmp(d->capability_id, "voice.channel")) {descriptor=d; ops=*o;} else {threshold_descriptor=d; threshold_ops=*o;} return CMPP_OK; }
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

esp_err_t voice_media_set_barge_threshold(voice_media_t *m, unsigned rms, bool dry, bool *changed)
{
    (void)m; *changed = false;
    if (current.playing) return ESP_ERR_INVALID_STATE;
    if (failure) return failure;
    if (!dry) { *changed = current.barge_min_rms != rms; current.barge_min_rms = rms; }
    return ESP_OK;
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
    assert(!cmpp_value_object_get(result.data, "wake_available"));
    cmpp_value_t state;
    current.conversation_active = true;
    current.followup_timeout_ms = 30000;
    assert(ops.observe(context, &state) == CMPP_OK);
    assert(state.as.object.count == 32);
    assert(cmpp_value_object_get(&state, "echo_average_us")->as.integer == 0);
    assert(!cmpp_value_object_get(&state, "stop_supported")->as.boolean);
    assert(!cmpp_value_object_get(&state, "echo_available")->as.boolean);
    assert(cmpp_value_object_get(&state, "conversation_active")->as.boolean);
    assert(cmpp_value_object_get(&state, "followup_timeout_ms")->as.integer == 30000);
    assert(!cmpp_value_object_get(&state, "wake_available")->as.boolean);
    assert(!strcmp(cmpp_value_object_get(&state, "wake_status")->as.string, "model_not_configured"));
    assert(!strcmp(cmpp_value_object_get(&state, "wake_word")->as.string, "小白小白"));
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
    assert(threshold_descriptor->command_type_count == 1);
    assert(!strcmp(threshold_descriptor->command_types[0], "voice.channel.barge_threshold.set"));
    assert(!strstr(descriptor->arguments_schema_json, "rms"));
    failure = ESP_OK;
    entry.key = "rms"; volume.as.integer = 1000;
    request.command_type = "voice.channel.barge_threshold.set"; request.dry_run = true;
    assert(threshold_ops.validate(context, &args) == CMPP_OK);
    assert(threshold_ops.execute(context, &request, &result) == CMPP_OK);
    assert(result.data->as.object.count == 1 && result.effect == CMPP_EFFECT_NO_EFFECT);
    assert(cmpp_value_object_get(result.data, "barge_min_rms")->as.integer == 1000);
    volume.as.integer = 450; request.dry_run = false;
    assert(threshold_ops.execute(context, &request, &result) == CMPP_OK);
    assert(result.effect == CMPP_EFFECT_COMMITTED && current.barge_min_rms == 450);
    current.playing = true;
    assert(threshold_ops.execute(context, &request, &result) == CMPP_ERR_CONFLICT);
    assert(!strcmp(result.error_code, "voice_busy"));
    current.playing = false;
    for (int i=0;i<2;++i) {
        volume.as.integer = i ? 8000 : 80;
        assert(threshold_ops.validate(context, &args) == CMPP_OK);
        volume.as.integer = i ? 8001 : 79;
        assert(threshold_ops.validate(context, &args) != CMPP_OK);
    }
    volume.type = CMPP_VALUE_DOUBLE; volume.as.number = 80.5;
    assert(threshold_ops.validate(context, &args) != CMPP_OK);
    entry.key = "volume";
    assert(threshold_ops.validate(context, &args) != CMPP_OK);
    board_runtime_destroy(b);
    puts("voice board: command shape, dry-run, result volume, preserved state and partial failure passed");
    return 0;
}
