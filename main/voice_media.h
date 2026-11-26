/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "device_identity.h"
#include "voice_diagnostics.h"
#include "voice_volume.h"
#include "voice_echo.h"

typedef struct voice_media voice_media_t;
typedef struct {
    char channel_id[33];
    bool ready;
    bool recording;
    bool playing;
    bool awaiting_response;
    bool fault;
    const char *last_fault_reason;
    const char *last_stop_reason;
    voice_diagnostics_t diagnostics;
    int volume;
    voice_codec_readback_t codec_readback;
    bool wake_available;
    const char *wake_status, *wake_word;
    bool conversation_active;
    unsigned followup_timeout_ms;
    unsigned barge_min_rms;
    bool stop_supported, stop_pending;
    voice_echo_stats_t echo;
} voice_media_state_t;

esp_err_t voice_media_create(voice_media_t **output);
esp_err_t voice_media_start(voice_media_t *media, const device_identity_t *identity);
esp_err_t voice_media_open(voice_media_t *media, bool dry_run);
esp_err_t voice_media_close(voice_media_t *media, const char *channel_id,
                           bool dry_run, bool *changed);
esp_err_t voice_media_set_volume(voice_media_t *media, int volume, bool dry_run, bool *changed);
esp_err_t voice_media_set_barge_threshold(voice_media_t *media, unsigned rms, bool dry_run, bool *changed);
esp_err_t voice_media_stop(voice_media_t *media);
void voice_media_observe(voice_media_t *media, voice_media_state_t *state);
