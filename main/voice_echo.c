/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_echo.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static aec_handle_t *s_aec;
static int16_t *s_mic, *s_ref, *s_out, *s_fifo, *s_mic_fifo, *s_ref_fifo;
static unsigned s_size, s_fill, s_head, s_count, s_capacity;
#define ECHO_WINDOW_FRAMES 50 /* One second, including normal speech pauses. */
typedef struct { uint64_t mic, output; bool reference; } echo_energy_t;
static echo_energy_t s_window[ECHO_WINDOW_FRAMES];
static unsigned s_window_next, s_window_frames, s_reference_frames;
static uint64_t s_window_mic, s_window_output;
static uint64_t s_total_us, s_processed_frames;
static voice_echo_stats_t s_stats;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

voice_echo_stats_t voice_echo_stats(void)
{
    portENTER_CRITICAL(&s_lock);
    voice_echo_stats_t result = s_stats;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

bool voice_echo_init(void)
{
    aec_config_t config = {.mic_num = 1, .ref_num = 1, .out_num = 1,
        .filter_length = 4, .sample_rate = VOICE_SAMPLE_RATE,
        .caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        .mode = AEC_MODE_FD_LOW_COST, .nlp_level = AEC_NLP_LEVEL_AGGR};
    s_aec = aec_create_from_config(&config);
    if (!s_aec) return false;
    s_size = aec_get_chunksize(s_aec);
    if (!s_size || s_size > 1024) goto failed;
    s_capacity = s_size + 2 * VOICE_FRAME_SAMPLES;
    s_mic = heap_caps_aligned_calloc(16, s_size, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ref = heap_caps_aligned_calloc(16, s_size, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_out = heap_caps_aligned_calloc(16, s_size, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_fifo = heap_caps_calloc(s_capacity, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mic_fifo = heap_caps_calloc(s_capacity, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ref_fifo = heap_caps_calloc(s_capacity, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mic || !s_ref || !s_out || !s_fifo || !s_mic_fifo || !s_ref_fifo) goto failed;
    /* One AEC frame of initial silence bridges its frame size to 20ms PCM. */
    s_count = s_size;
    s_stats.available = true;
    s_stats.frame_samples = s_size;
    ESP_LOGI("voice_echo", "FD AEC initialized: frame=%u samples, MIC1 + MIC3 reference", s_size);
    return true;
failed:
    free(s_mic); free(s_ref); free(s_out); free(s_fifo); free(s_mic_fifo); free(s_ref_fifo);
    s_mic = s_ref = s_out = s_fifo = s_mic_fifo = s_ref_fifo = NULL;
    aec_destroy(s_aec); s_aec = NULL;
    return false;
}

bool voice_echo_process(const int16_t *mic, const int16_t *reference, int16_t *output)
{
    if (!s_aec) return false;
    int64_t start = esp_timer_get_time();
    uint64_t mic_energy = 0, ref_energy = 0, out_energy = 0;
    for (unsigned i = 0; i < VOICE_FRAME_SAMPLES; ++i) {
        s_mic[s_fill] = mic[i]; s_ref[s_fill] = reference[i];
        if (++s_fill == s_size) {
            aec_process(s_aec, s_mic, s_ref, s_out);
            if (s_count + s_size > s_capacity) return false;
            for (unsigned j = 0; j < s_size; ++j) {
                unsigned index = (s_head + s_count + j) % s_capacity;
                s_fifo[index] = s_out[j];
                /* Delay the meter's inputs by the same bridge latency as
                 * output. Comparing output to the next syllable is invalid. */
                s_mic_fifo[index] = s_mic[j];
                s_ref_fifo[index] = s_ref[j];
            }
            s_count += s_size;
            s_fill = 0;
        }
    }
    if (s_count < VOICE_FRAME_SAMPLES) return false;
    for (unsigned i = 0; i < VOICE_FRAME_SAMPLES; ++i) {
        output[i] = s_fifo[s_head];
        mic_energy += (int64_t)s_mic_fifo[s_head] * s_mic_fifo[s_head];
        ref_energy += (int64_t)s_ref_fifo[s_head] * s_ref_fifo[s_head];
        s_head = (s_head + 1) % s_capacity;
        out_energy += (int64_t)output[i] * output[i];
    }
    s_count -= VOICE_FRAME_SAMPLES;
    unsigned mic_rms = sqrt((double)mic_energy / VOICE_FRAME_SAMPLES);
    unsigned ref_rms = sqrt((double)ref_energy / VOICE_FRAME_SAMPLES);
    unsigned out_rms = sqrt((double)out_energy / VOICE_FRAME_SAMPLES);
    /* Require >=200ms of actual reference in a bounded one-second window,
     * with >=6dB aggregate suppression on those same frames. Natural pauses
     * must not reset all evidence; no-reference speech must not supply it. */
    echo_energy_t old = s_window[s_window_next];
    s_window_mic -= old.mic; s_window_output -= old.output;
    s_reference_frames -= old.reference;
    echo_energy_t current = {.reference = ref_rms >= 64};
    if (current.reference) { current.mic = mic_energy; current.output = out_energy; }
    s_window[s_window_next] = current;
    s_window_next = (s_window_next + 1) % ECHO_WINDOW_FRAMES;
    if (s_window_frames < ECHO_WINDOW_FRAMES) ++s_window_frames;
    s_window_mic += current.mic; s_window_output += current.output;
    s_reference_frames += current.reference;
    bool qualified = s_window_frames == ECHO_WINDOW_FRAMES && s_reference_frames >= 10 &&
        s_window_mic >= (uint64_t)128 * 128 * VOICE_FRAME_SAMPLES * s_reference_frames &&
        s_window_output * 4 < s_window_mic;
    unsigned elapsed = esp_timer_get_time() - start;
    s_total_us += elapsed;
    ++s_processed_frames;
    portENTER_CRITICAL(&s_lock);
    s_stats.mic_rms = mic_rms; s_stats.reference_rms = ref_rms; s_stats.output_rms = out_rms;
    if (elapsed > s_stats.worst_us) s_stats.worst_us = elapsed;
    s_stats.average_us = s_total_us / s_processed_frames;
    bool became_ready = qualified && !s_stats.ready;
    if (qualified) s_stats.ready = true;
    portEXIT_CRITICAL(&s_lock);
    if (became_ready) ESP_LOGI("voice_echo", "Reference verified: active=%u/50 suppression_power_pct=%u",
        s_reference_frames, (unsigned)(s_window_output * 100 / s_window_mic));
    else if (!s_window_next && s_reference_frames >= 2 && s_window_mic)
        ESP_LOGI("voice_echo", "Reference window: active=%u/50 suppression_power_pct=%u ready=%d",
            s_reference_frames, (unsigned)(s_window_output * 100 / s_window_mic), voice_echo_stats().ready);
    return true;
}
