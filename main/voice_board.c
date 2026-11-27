/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_board.h"
#include "voice_audio.h"
#include "voice_media.h"
#include "voice_wire.h"
#include <stdlib.h>
#include <string.h>

struct board_runtime {
    cmpp_runtime_t *runtime;
    voice_media_t *media;
    char reply_id[33];
    cmpp_value_t reply_values[32];
    cmpp_object_entry_t reply_entries[32];
    cmpp_value_t diagnostic_values[19];
    cmpp_object_entry_t diagnostic_entries[19];
    cmpp_value_t codec_values[13], boot_values[VOICE_CODEC_BOOT_REG_COUNT], boot_errors[VOICE_CODEC_BOOT_REG_COUNT];
    cmpp_object_entry_t codec_entries[13], boot_value_entries[VOICE_CODEC_BOOT_REG_COUNT], boot_error_entries[VOICE_CODEC_BOOT_REG_COUNT];
    cmpp_value_t reply;
    cmpp_value_t threshold_reply, threshold_value;
    cmpp_object_entry_t threshold_entry;
};

static const char *const COMMANDS[] = {"voice.channel.open", "voice.channel.close", "voice.channel.volume.set"};
static const cmpp_capability_descriptor_t DESCRIPTOR = {
    .capability_id = "voice.channel", .display_name = "Voice PCM channel",
    .version = "1.1.0", .description = "Single PCM16LE 16kHz channel; PTT and optional local wake; development LAN transport",
    .arguments_schema_json = "{\"oneOf\":[{\"type\":\"object\",\"maxProperties\":0},{\"type\":\"object\",\"required\":[\"channel_id\"],\"additionalProperties\":false,\"properties\":{\"channel_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64}}},{\"type\":\"object\",\"required\":[\"volume\"],\"additionalProperties\":false,\"properties\":{\"volume\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}}}]}",
    .state_schema_json = "{\"type\":\"object\"}",
    .command_types = COMMANDS, .command_type_count = 3,
    .allowed_safety_mask = CMPP_SAFETY_MASK(CMPP_SAFETY_REVERSIBLE),
    .requires_ack = true, .supports_dry_run = true,
};

static cmpp_status_t validate(void *context, const cmpp_value_t *args)
{
    (void)context;
    if (!args || args->type != CMPP_VALUE_OBJECT) return CMPP_ERR_INVALID_ARGUMENT;
    if (args->as.object.count == 0) return CMPP_OK;
    const cmpp_value_t *volume = cmpp_value_object_get(args, "volume");
    if (volume) return args->as.object.count == 1 && volume->type == CMPP_VALUE_INT &&
        volume->as.integer >= 0 && volume->as.integer <= 100 ? CMPP_OK : CMPP_ERR_INVALID_ARGUMENT;
    const cmpp_value_t *id = cmpp_value_object_get(args, "channel_id");
    return args->as.object.count == 1 && id && id->type == CMPP_VALUE_STRING && voice_id_valid(id->as.string)
        ? CMPP_OK : CMPP_ERR_INVALID_ARGUMENT;
}

static cmpp_status_t admit(void *context, const cmpp_request_t *request)
{
    (void)context;
    if (!strcmp(request->command_type, COMMANDS[0]))
        return request->arguments->as.object.count == 0 ? CMPP_OK : CMPP_ERR_INVALID_ARGUMENT;
    const char *key = !strcmp(request->command_type, COMMANDS[1]) ? "channel_id" :
                      !strcmp(request->command_type, COMMANDS[2]) ? "volume" : NULL;
    return key && cmpp_value_object_get(request->arguments, key) ? CMPP_OK : CMPP_ERR_INVALID_ARGUMENT;
}

static void snapshot(board_runtime_t *b, bool detailed)
{
    voice_media_state_t state;
    voice_media_observe(b->media, &state);
    strcpy(b->reply_id, state.channel_id);
    const char *keys[] = {"channel_id", "ws_path", "ws_port", "format", "sample_rate", "channels", "frame_ms", "ready", "recording", "playing", "awaiting_response", "fault", "last_fault_reason", "last_stop_reason", "diagnostics", "volume", "codec_readback", "wake_available", "wake_status", "wake_word", "conversation_active", "followup_timeout_ms", "stop_supported", "stop_pending", "echo_available", "echo_ready", "mic_rms", "reference_rms", "clean_rms", "echo_worst_us", "echo_average_us", "barge_min_rms"};
    b->reply_values[0] = (cmpp_value_t){.type = CMPP_VALUE_STRING, .as.string = b->reply_id};
    b->reply_values[1] = (cmpp_value_t){.type = CMPP_VALUE_STRING, .as.string = "/voice"};
    b->reply_values[2] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = CONFIG_CMPP_VOICE_HTTP_PORT};
    b->reply_values[3] = (cmpp_value_t){.type = CMPP_VALUE_STRING, .as.string = "pcm_s16le"};
    b->reply_values[4] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = VOICE_SAMPLE_RATE};
    b->reply_values[5] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = 1};
    b->reply_values[6] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = 20};
    b->reply_values[7] = (cmpp_value_t){.type = CMPP_VALUE_BOOL, .as.boolean = state.ready};
    b->reply_values[8] = (cmpp_value_t){.type = CMPP_VALUE_BOOL, .as.boolean = state.recording};
    b->reply_values[9] = (cmpp_value_t){.type = CMPP_VALUE_BOOL, .as.boolean = state.playing};
    b->reply_values[10] = (cmpp_value_t){.type = CMPP_VALUE_BOOL, .as.boolean = state.awaiting_response};
    b->reply_values[11] = (cmpp_value_t){.type = CMPP_VALUE_BOOL, .as.boolean = state.fault};
    b->reply_values[12] = (cmpp_value_t){.type = CMPP_VALUE_STRING, .as.string = state.last_fault_reason};
    b->reply_values[13] = (cmpp_value_t){.type = CMPP_VALUE_STRING, .as.string = state.last_stop_reason};
    b->reply_values[15] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = state.volume};
    /* Command results are retained by the idempotency cache and SSE replay.
     * Keep diagnostic trees in the read-only state resource only. */
    if (!detailed) {
        for (size_t i = 0; i < 14; ++i)
            b->reply_entries[i] = (cmpp_object_entry_t){.key = keys[i], .value = &b->reply_values[i]};
        b->reply_entries[14] = (cmpp_object_entry_t){.key = "volume", .value = &b->reply_values[15]};
        b->reply = (cmpp_value_t){.type = CMPP_VALUE_OBJECT,
            .as.object = {.entries = b->reply_entries, .count = 15}};
        return;
    }
    b->reply_values[17] = (cmpp_value_t){.type = CMPP_VALUE_BOOL, .as.boolean = state.wake_available};
    b->reply_values[18] = (cmpp_value_t){.type = CMPP_VALUE_STRING, .as.string = state.wake_status ? state.wake_status : "unavailable"};
    b->reply_values[19] = (cmpp_value_t){.type = CMPP_VALUE_STRING, .as.string = state.wake_word ? state.wake_word : ""};
    b->reply_values[20] = (cmpp_value_t){.type = CMPP_VALUE_BOOL, .as.boolean = state.conversation_active};
    b->reply_values[21] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = state.followup_timeout_ms};
    const bool flags[] = {state.stop_supported, state.stop_pending, state.echo.available, state.echo.ready};
    for (size_t i = 0; i < 4; ++i)
        b->reply_values[22+i] = (cmpp_value_t){.type = CMPP_VALUE_BOOL, .as.boolean = flags[i]};
    const unsigned levels[] = {state.echo.mic_rms, state.echo.reference_rms, state.echo.output_rms, state.echo.worst_us};
    for (size_t i = 0; i < 4; ++i)
        b->reply_values[26+i] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = levels[i]};
    b->reply_values[30] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = state.echo.average_us};
    b->reply_values[31] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = state.barge_min_rms};
    const voice_diagnostics_t *d = &state.diagnostics;
    const char *diagnostic_keys[] = {
        "upload_pcm_queued_bytes", "upload_pcm_sent_bytes", "output_pcm_received_bytes",
        "output_pcm_i2s_bytes", "upload_queue_high_water", "upload_queue_full_count",
        "upload_work_schedule_failed_count", "upload_send_failed_count", "upload_send_max_us",
        "last_upload_failure_generation", "last_upload_schedule_error", "last_upload_schedule_errno",
        "last_upload_failure_packet", "last_upload_failure_reason", "scope", "configured_output_volume",
        "upload_pcm_dropped_frames", "upload_pcm_dropped_bytes", "upload_control_queue_full_count"
    };
    const int64_t numbers[] = {
        d->upload_pcm_queued_bytes, d->upload_pcm_sent_bytes, d->output_pcm_received_bytes,
        d->output_pcm_i2s_bytes, d->upload_queue_high_water, d->upload_queue_full_count,
        d->upload_work_schedule_failed_count, d->upload_send_failed_count, d->upload_send_max_us,
        d->last_upload_failure_generation, d->last_upload_schedule_error, d->last_upload_schedule_errno
    };
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i)
        b->diagnostic_values[i] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = numbers[i]};
    b->diagnostic_values[12] = (cmpp_value_t){.type = CMPP_VALUE_STRING,
        .as.string = d->last_upload_failure_packet ? d->last_upload_failure_packet : "none"};
    b->diagnostic_values[13] = (cmpp_value_t){.type = CMPP_VALUE_STRING,
        .as.string = voice_stop_reason_name(d->last_upload_failure)};
    b->diagnostic_values[14] = (cmpp_value_t){.type = CMPP_VALUE_STRING, .as.string = "since_boot"};
    /* Build-time fallback only; current accepted configuration is state.volume. */
    b->diagnostic_values[15] = (cmpp_value_t){.type = CMPP_VALUE_INT,
        .as.integer = CONFIG_CMPP_VOICE_OUTPUT_VOLUME};
    b->diagnostic_values[16] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = d->upload_pcm_dropped_frames};
    b->diagnostic_values[17] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = d->upload_pcm_dropped_bytes};
    b->diagnostic_values[18] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = d->upload_control_queue_full_count};
    for (size_t i = 0; i < sizeof(diagnostic_keys) / sizeof(diagnostic_keys[0]); ++i)
        b->diagnostic_entries[i] = (cmpp_object_entry_t){.key = diagnostic_keys[i], .value = &b->diagnostic_values[i]};
    b->reply_values[14] = (cmpp_value_t){.type = CMPP_VALUE_OBJECT,
        .as.object = {.entries = b->diagnostic_entries, .count = 19}};
    b->reply_values[15] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = state.volume};
    const voice_codec_readback_t *c = &state.codec_readback;
    const char *register_keys[] = {"0x00", "0x01", "0x02", "0x03", "0x04", "0x05", "0x06", "0x07",
                                   "0x08", "0x09", "0x0d", "0x0e", "0x12", "0x31", "0x32", "0x37"};
    for (size_t i = 0; i < VOICE_CODEC_BOOT_REG_COUNT; ++i) {
        b->boot_values[i] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = c->boot_values[i]};
        b->boot_errors[i] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = c->boot_errors[i]};
        b->boot_value_entries[i] = (cmpp_object_entry_t){.key = register_keys[i], .value = &b->boot_values[i]};
        b->boot_error_entries[i] = (cmpp_object_entry_t){.key = register_keys[i], .value = &b->boot_errors[i]};
    }
    const char *codec_keys[] = {"boot_values", "boot_errors", "mute_reg31", "mute_error", "volume_reg32",
        "volume_error", "last_unmute_reg31", "last_unmute_error", "read_count", "read_failed_count",
        "unmute_read_count", "requested_mute", "source"};
    b->codec_values[0] = (cmpp_value_t){.type = CMPP_VALUE_OBJECT,
        .as.object = {.entries = b->boot_value_entries, .count = VOICE_CODEC_BOOT_REG_COUNT}};
    b->codec_values[1] = (cmpp_value_t){.type = CMPP_VALUE_OBJECT,
        .as.object = {.entries = b->boot_error_entries, .count = VOICE_CODEC_BOOT_REG_COUNT}};
    const int64_t codec_numbers[] = {c->mute_reg31, c->mute_error, c->volume_reg32, c->volume_error,
        c->last_unmute_reg31, c->last_unmute_error, c->read_count, c->read_failed_count, c->unmute_read_count};
    for (size_t i = 0; i < sizeof(codec_numbers) / sizeof(codec_numbers[0]); ++i)
        b->codec_values[i + 2] = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = codec_numbers[i]};
    b->codec_values[11] = (cmpp_value_t){.type = CMPP_VALUE_BOOL, .as.boolean = c->requested_mute};
    b->codec_values[12] = (cmpp_value_t){.type = CMPP_VALUE_STRING, .as.string = "es8311_i2c_cached"};
    for (size_t i = 0; i < sizeof(codec_keys) / sizeof(codec_keys[0]); ++i)
        b->codec_entries[i] = (cmpp_object_entry_t){.key = codec_keys[i], .value = &b->codec_values[i]};
    b->reply_values[16] = (cmpp_value_t){.type = CMPP_VALUE_OBJECT,
        .as.object = {.entries = b->codec_entries, .count = 13}};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        b->reply_entries[i] = (cmpp_object_entry_t){.key = keys[i], .value = &b->reply_values[i]};
    b->reply = (cmpp_value_t){.type = CMPP_VALUE_OBJECT,
        .as.object = {.entries = b->reply_entries, .count = sizeof(keys) / sizeof(keys[0])}};
}

static cmpp_status_t execute(void *context, const cmpp_request_t *request, cmpp_result_t *result)
{
    board_runtime_t *b = context;
    bool opening = strcmp(request->command_type, COMMANDS[0]) == 0;
    bool volume_setting = strcmp(request->command_type, COMMANDS[2]) == 0;
    bool changed = false;
    esp_err_t err;
    if (volume_setting) {
        int volume = (int)cmpp_value_object_get(request->arguments, "volume")->as.integer;
        err = voice_media_set_volume(b->media, volume, request->dry_run, &changed);
    } else if (opening) {
        err = voice_media_open(b->media, request->dry_run);
        changed = err == ESP_OK && !request->dry_run;
    } else {
        const char *id = cmpp_value_object_get(request->arguments, "channel_id")->as.string;
        err = voice_media_close(b->media, id, request->dry_run, &changed);
    }
    snapshot(b, false);
    result->data = &b->reply;
    result->outcome = err == ESP_OK ? CMPP_OUTCOME_COMPLETED :
        err == ESP_ERR_INVALID_STATE ? CMPP_OUTCOME_REJECTED : CMPP_OUTCOME_FAILED;
    result->effect = err != ESP_OK && err != ESP_ERR_INVALID_STATE ? CMPP_EFFECT_UNKNOWN :
        changed ? CMPP_EFFECT_COMMITTED : CMPP_EFFECT_NO_EFFECT;
    result->error_code = err == ESP_OK ? NULL : err == ESP_ERR_INVALID_STATE ? "channel_conflict" : volume_setting ? "volume_set_failed" : "audio_stop_failed";
    return err == ESP_OK ? CMPP_OK : err == ESP_ERR_INVALID_STATE ? CMPP_ERR_CONFLICT : CMPP_ERR_IO;
}

static cmpp_status_t observe(void *context, cmpp_value_t *state)
{
    board_runtime_t *b = context;
    snapshot(b, true);
    *state = b->reply;
    return CMPP_OK;
}

static cmpp_status_t safe_stop(void *context)
{
    board_runtime_t *b = context;
    esp_err_t err = voice_media_stop(b->media);
    return err == ESP_OK ? CMPP_OK : CMPP_ERR_IO;
}

static const char *const THRESHOLD_COMMANDS[] = {"voice.channel.barge_threshold.set"};
static const cmpp_capability_descriptor_t THRESHOLD_DESCRIPTOR = {
    .capability_id = "voice.barge_threshold", .display_name = "语音打断门槛",
    .version = "1.0.0", .description = "历史语音打断门槛；当前纯按键模式不使用此值，保留配置兼容",
    .arguments_schema_json = "{\"type\":\"object\",\"required\":[\"rms\"],\"additionalProperties\":false,\"properties\":{\"rms\":{\"type\":\"integer\",\"minimum\":80,\"maximum\":8000}}}",
    .state_schema_json = "{\"type\":\"object\"}",
    .command_types = THRESHOLD_COMMANDS, .command_type_count = 1,
    .allowed_safety_mask = CMPP_SAFETY_MASK(CMPP_SAFETY_REVERSIBLE),
    .requires_ack = true, .supports_dry_run = true,
};

static cmpp_status_t threshold_validate(void *context, const cmpp_value_t *args)
{
    (void)context;
    const cmpp_value_t *rms = cmpp_value_object_get(args, "rms");
    return args && args->type == CMPP_VALUE_OBJECT && args->as.object.count == 1 &&
        rms && rms->type == CMPP_VALUE_INT && rms->as.integer >= 80 && rms->as.integer <= 8000
        ? CMPP_OK : CMPP_ERR_INVALID_ARGUMENT;
}

static cmpp_status_t threshold_observe(void *context, cmpp_value_t *state)
{
    board_runtime_t *b = context;
    voice_media_state_t observed;
    voice_media_observe(b->media, &observed);
    b->threshold_value = (cmpp_value_t){.type = CMPP_VALUE_INT, .as.integer = observed.barge_min_rms};
    b->threshold_entry = (cmpp_object_entry_t){.key = "barge_min_rms", .value = &b->threshold_value};
    b->threshold_reply = (cmpp_value_t){.type = CMPP_VALUE_OBJECT,
        .as.object = {.entries = &b->threshold_entry, .count = 1}};
    *state = b->threshold_reply;
    return CMPP_OK;
}

static cmpp_status_t threshold_execute(void *context, const cmpp_request_t *request, cmpp_result_t *result)
{
    board_runtime_t *b = context;
    bool changed = false;
    unsigned rms = cmpp_value_object_get(request->arguments, "rms")->as.integer;
    esp_err_t err = voice_media_set_barge_threshold(b->media, rms, request->dry_run, &changed);
    cmpp_value_t state;
    threshold_observe(b, &state);
    result->data = &b->threshold_reply;
    result->outcome = err == ESP_OK ? CMPP_OUTCOME_COMPLETED :
        err == ESP_ERR_INVALID_STATE ? CMPP_OUTCOME_REJECTED : CMPP_OUTCOME_FAILED;
    result->effect = err != ESP_OK && err != ESP_ERR_INVALID_STATE ? CMPP_EFFECT_UNKNOWN :
        changed ? CMPP_EFFECT_COMMITTED : CMPP_EFFECT_NO_EFFECT;
    result->error_code = err == ESP_OK ? NULL : err == ESP_ERR_INVALID_STATE ? "voice_busy" : "threshold_save_failed";
    return err == ESP_OK ? CMPP_OK : err == ESP_ERR_INVALID_STATE ? CMPP_ERR_CONFLICT : CMPP_ERR_IO;
}

esp_err_t board_runtime_create(cmpp_runtime_t *runtime, board_runtime_t **output)
{
    if (!runtime || !output) return ESP_ERR_INVALID_ARG;
    *output = NULL;
    board_runtime_t *b = calloc(1, sizeof(*b));
    if (!b) return ESP_ERR_NO_MEM;
    b->runtime = runtime;
    esp_err_t err = voice_media_create(&b->media);
    if (err != ESP_OK) { free(b); return err; }
    const cmpp_capability_ops_t ops = {
        .validate = validate, .admit = admit, .execute = execute,
        .observe = observe, .safe_stop = safe_stop,
    };
    if (cmpp_runtime_register_capability(runtime, &DESCRIPTOR, &ops, b) != CMPP_OK) {
        (void)voice_media_stop(b->media);
        free(b);
        return ESP_FAIL;
    }
    const cmpp_capability_ops_t threshold_ops = {
        .validate = threshold_validate, .execute = threshold_execute, .observe = threshold_observe,
    };
    if (cmpp_runtime_register_capability(runtime, &THRESHOLD_DESCRIPTOR, &threshold_ops, b) != CMPP_OK) {
        (void)cmpp_runtime_unregister_capability(runtime, "voice.channel");
        (void)voice_media_stop(b->media);
        free(b);
        return ESP_FAIL;
    }
    *output = b;
    return ESP_OK;
}

esp_err_t board_runtime_safe_stop(board_runtime_t *b)
{
    return b ? voice_media_stop(b->media) : ESP_ERR_INVALID_ARG;
}

void board_runtime_destroy(board_runtime_t *b)
{
    if (!b) return;
    (void)cmpp_runtime_unregister_capability(b->runtime, "voice.barge_threshold");
    (void)cmpp_runtime_unregister_capability(b->runtime, "voice.channel");
    (void)voice_media_stop(b->media);
    /* The media tasks own boot-lifetime state, not this capability wrapper. */
    free(b);
}

esp_err_t voice_board_start(board_runtime_t *b, const device_identity_t *identity)
{
    return b ? voice_media_start(b->media, identity) : ESP_ERR_INVALID_ARG;
}
