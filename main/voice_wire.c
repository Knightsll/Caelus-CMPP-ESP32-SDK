/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_wire.h"
#include <string.h>
#include "cJSON.h"

bool voice_id_valid(const char *id)
{
    if (!id || !*id || strlen(id) > VOICE_ID_MAX) return false;
    for (const char *p = id; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    }
    return true;
}

static const char *string_field(cJSON *root, const char *key)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

bool voice_wire_output_blocked(const voice_wire_t *s)
{
    return s->awaiting_stop || (s->interrupt_enabled && (s->awaiting_accept || s->input_active));
}

bool voice_wire_begin_stop(voice_wire_t *s, const char *id)
{
    if (!s || !s->ready || !s->stop_enabled || !voice_id_valid(id)) return false;
    strcpy(s->stop_id, id);
    s->awaiting_stop = true;
    s->input_active = s->awaiting_accept = s->output = s->tail = false;
    return true;
}

bool voice_wire_begin_input(voice_wire_t *s, const char *turn)
{
    if (!s || !s->ready || s->awaiting_stop || !voice_id_valid(turn)) return false;
    if (s->interrupt_enabled) {
        strcpy(s->input_turn, turn);
        s->input_active = s->awaiting_accept = true;
        s->output = s->tail = false;
    }
    return true;
}

void voice_wire_end_input(voice_wire_t *s)
{
    s->input_active = false;
}

voice_message_t voice_wire_text(voice_wire_t *s, const char *channel,
                                const char *json, size_t size)
{
    if (!s || !channel || !*channel || !json || size == 0 || size > VOICE_JSON_BYTES ||
        memchr(json, '\0', size)) return VOICE_BAD;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, size, &end, false);
    voice_message_t result = VOICE_BAD;
    if (!cJSON_IsObject(root)) goto done;
    while (end < json + size && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) ++end;
    if (end != json + size) goto done;
    const char *type = string_field(root, "type");
    if (!type) goto done;
    if (!s->ready) {
        const char *protocol = string_field(root, "protocol");
        const char *id = string_field(root, "channel_id");
        if (cJSON_GetArraySize(root) == 3 && strcmp(type, "hello") == 0 &&
            protocol && strcmp(protocol, VOICE_PROTOCOL) == 0 && id && strcmp(id, channel) == 0) {
            s->ready = true;
            result = VOICE_HELLO;
        }
        goto done;
    }
    if (strcmp(type, "session.configure") == 0) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "interrupt");
        const cJSON *stop = cJSON_GetObjectItemCaseSensitive(root, "session_stop");
        int fields = 1 + (enabled != NULL) + (stop != NULL);
        if (fields >= 2 && cJSON_GetArraySize(root) == fields &&
            (!enabled || cJSON_IsBool(enabled)) && (!stop || cJSON_IsBool(stop)) &&
            !s->output && !s->input_active && !s->awaiting_accept && !s->awaiting_stop) {
            s->interrupt_enabled = cJSON_IsTrue(enabled);
            s->stop_enabled = cJSON_IsTrue(stop);
            result = VOICE_CONFIGURED;
        }
        goto done;
    }
    if (strcmp(type, "input.accepted") == 0) {
        const char *turn = string_field(root, "turn_id");
        if (cJSON_GetArraySize(root) == 2 && voice_id_valid(turn) && s->interrupt_enabled) {
            result = VOICE_IGNORED;
            if (s->awaiting_accept && strcmp(turn, s->input_turn) == 0) {
                s->awaiting_accept = false;
                result = VOICE_INPUT_ACCEPTED;
            }
        }
        goto done;
    }
    if (strcmp(type, "session.state") == 0) {
        const char *state = string_field(root, "state");
        const char *stop = string_field(root, "stop_id");
        if (cJSON_GetArraySize(root) == 3 && state && !strcmp(state, "listening") &&
            voice_id_valid(stop) && s->stop_enabled) {
            result = VOICE_IGNORED;
            if (s->awaiting_stop && !strcmp(stop, s->stop_id)) {
                s->awaiting_stop = false;
                result = VOICE_SESSION_STOPPED;
            }
            goto done;
        }
        if (cJSON_GetArraySize(root) == 2 && state) {
            if (strcmp(state, "thinking") == 0) result = VOICE_SESSION_THINKING;
            else if (strcmp(state, "listening") == 0) result = VOICE_SESSION_LISTENING;
            if (result != VOICE_BAD && voice_wire_output_blocked(s)) result = VOICE_IGNORED;
        }
        goto done;
    }
    const char *turn = string_field(root, "turn_id");
    if (cJSON_GetArraySize(root) != 2 || !voice_id_valid(turn)) goto done;
    if (voice_wire_output_blocked(s) &&
        (strcmp(type, "output.start") == 0 || strcmp(type, "output.end") == 0)) {
        result = VOICE_IGNORED;
        goto done;
    }
    if (strcmp(type, "output.start") == 0 && !s->output && strcmp(turn, s->turn_id) != 0) {
        strcpy(s->turn_id, turn);
        s->output = true;
        s->tail = false;
        result = VOICE_OUTPUT_START;
    } else if (strcmp(type, "output.end") == 0 && s->output && strcmp(turn, s->turn_id) == 0) {
        s->output = false;
        result = VOICE_OUTPUT_END;
    }
done:
    cJSON_Delete(root);
    return result;
}

bool voice_wire_pcm(voice_wire_t *s, size_t size)
{
    if (!s || !s->ready || !s->output || s->tail || size == 0 ||
        size > VOICE_PCM_BYTES || size % 2) return false;
    s->tail = size < VOICE_PCM_BYTES;
    return true;
}

voice_ptt_event_t voice_ptt_step(voice_ptt_t *s, bool ready, bool output_busy,
                                bool recording, bool pressed)
{
    voice_ptt_event_t event = VOICE_PTT_NONE;
    if (!ready || output_busy) s->armed = false;
    else if (!pressed) s->armed = true;
    if (ready && !output_busy) {
        if (!recording && s->armed && pressed && s->previous_pressed) {
            s->armed = false;
            event = VOICE_PTT_START;
        } else if (recording && !pressed && !s->previous_pressed) {
            event = VOICE_PTT_END;
        }
    }
    s->previous_pressed = pressed;
    return event;
}
