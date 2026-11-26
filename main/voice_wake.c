/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_wake.h"
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "voice_micro.h"
#include "esp_vad.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define WAKE_QUEUE_FRAMES 12
typedef struct {
    uint32_t epoch;
    voice_wake_mode_t mode;
    int16_t pcm[VOICE_FRAME_SAMPLES];
} wake_frame_t;
typedef struct { uint32_t epoch; voice_wake_event_t event; } wake_result_t;

struct voice_wake {
    QueueHandle_t frames, results;
    StaticQueue_t frame_control;
    uint8_t *frame_storage;
    TaskHandle_t task;
    voice_micro_t *model;
    vad_handle_t vad;
    /* These fields belong exclusively to capture_task. */
    uint32_t epoch, generation, dropped;
    voice_wake_mode_t mode;
    unsigned barge_min_rms;
};
static const char *TAG = "voice_wake";

static void wake_task(void *arg)
{
    voice_wake_t *w = arg;
    wake_frame_t frame;
    uint32_t epoch = 0;
    bool reported = false;
    voice_endpoint_t endpoint = {0};
    voice_onset_t onset = {0};
    double noise_power = 6400;
    unsigned diagnostic_frames = 0, diagnostic_vad = 0, diagnostic_energy = 0;
    double diagnostic_peak = 0;
    while (xQueueReceive(w->frames, &frame, portMAX_DELAY) == pdTRUE) {
        if (frame.epoch != epoch) {
            epoch = frame.epoch;
            reported = false;
            endpoint = (voice_endpoint_t){0};
            onset = (voice_onset_t){0};
            diagnostic_frames = diagnostic_vad = diagnostic_energy = 0;
            diagnostic_peak = 0;
            voice_micro_reset(w->model);
            vad_reset_trigger(w->vad);
        }
        if (reported) continue;
        double power = 0;
        for (unsigned i = 0; i < VOICE_FRAME_SAMPLES; ++i)
            power += (double)frame.pcm[i] * frame.pcm[i];
        power /= VOICE_FRAME_SAMPLES;
        voice_wake_event_t event = VOICE_WAKE_NONE;
        if (frame.mode == VOICE_WAKE_CAPTURE) {
            bool speech = vad_process(w->vad, frame.pcm, VOICE_SAMPLE_RATE, 20) == VAD_SPEECH;
            if (voice_endpoint_step(&endpoint, speech, CONFIG_CMPP_VOICE_WAKE_SILENCE_MS))
                event = VOICE_WAKE_END;
        } else if (frame.mode == VOICE_WAKE_FOLLOWUP || frame.mode == VOICE_WAKE_BARGE) {
            bool speech = vad_process(w->vad, frame.pcm, VOICE_SAMPLE_RATE, 20) == VAD_SPEECH;
            ++diagnostic_frames;
            diagnostic_vad += speech;
            unsigned min_rms = frame.mode == VOICE_WAKE_BARGE ?
                __atomic_load_n(&w->barge_min_rms, __ATOMIC_RELAXED) : 80;
            diagnostic_energy += power >= (double)min_rms * min_rms && power >= noise_power * 4;
            if (power > diagnostic_peak) diagnostic_peak = power;
            if (diagnostic_frames == 50) {
                ESP_LOGI(TAG, "Listen mode=%d vad=%u/50 energy=%u/50 peak=%.0f floor=%.0f min_rms=%u",
                    frame.mode, diagnostic_vad, diagnostic_energy, diagnostic_peak, noise_power, min_rms);
                diagnostic_frames = diagnostic_vad = diagnostic_energy = 0;
                diagnostic_peak = 0;
            }
            if (voice_followup_step(&onset, speech, power, noise_power,
                                    frame.mode == VOICE_WAKE_BARGE, min_rms)) {
                event = VOICE_WAKE_START;
                ESP_LOGI(TAG, "Speech onset mode=%d duration=%u power=%.0f floor=%.0f",
                    frame.mode, onset.consecutive_ms, power, noise_power);
            }
        } else if (frame.mode == VOICE_WAKE_LISTEN) {
            /* Learn only while waiting for the keyword. Fast decay, bounded
             * slow rise keeps the keyword from setting the noise floor. */
            double bounded = power < noise_power * 4 ? power : noise_power * 4;
            noise_power += (bounded - noise_power) * (power < noise_power ? 0.05 : 0.002);
            if (noise_power < 1) noise_power = 1;
            int result = voice_micro_process(w->model, frame.pcm, VOICE_FRAME_SAMPLES);
            if (result > 0) event = VOICE_WAKE_START;
            else if (result < 0) {
                ESP_LOGE(TAG, "Wake inference failed; skip epoch, PTT remains usable");
                reported = true;
            }
        }
        if (event != VOICE_WAKE_NONE) {
            wake_result_t result = {.epoch = epoch, .event = event};
            /* One result per epoch. A full mailbox contains only older results. */
            xQueueOverwrite(w->results, &result);
            reported = true;
        }
    }
    vTaskDelete(NULL);
}

void voice_wake_destroy(voice_wake_t *w)
{
    if (!w) return;
    if (w->task) vTaskDelete(w->task);
    if (w->frames) vQueueDelete(w->frames);
    if (w->results) vQueueDelete(w->results);
    if (w->vad) vad_destroy(w->vad);
    voice_micro_destroy(w->model);
    free(w->frame_storage);
    free(w);
}

const char *voice_wake_create(voice_wake_t **output)
{
    *output = NULL;
    /* An empty name never silently substitutes a bundled wake word. */
    if (!CONFIG_CMPP_VOICE_WAKE_MODEL[0]) return "model_not_configured";
    voice_wake_t *w = calloc(1, sizeof(*w));
    if (!w) return "no_memory";
    w->barge_min_rms = CONFIG_CMPP_VOICE_BARGE_MIN_RMS;
    const char *status = voice_micro_create(&w->model);
    if (!w->model) goto failed;
    status = "no_memory";
    w->frame_storage = heap_caps_malloc(WAKE_QUEUE_FRAMES * sizeof(wake_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    w->vad = vad_create_with_param(VAD_MODE_3, VOICE_SAMPLE_RATE, 20, 100, 100);
    w->results = xQueueCreate(1, sizeof(wake_result_t));
    if (!w->frame_storage || !w->vad || !w->results) goto failed;
    w->frames = xQueueCreateStatic(WAKE_QUEUE_FRAMES, sizeof(wake_frame_t), w->frame_storage, &w->frame_control);
    if (!w->frames) goto failed;
    /* Lower priority than capture/playback; never run inference under media lock. */
    if (xTaskCreate(wake_task, "voice_wake", 8192, w, 4, &w->task) != pdPASS) goto failed;
    *output = w;
    ESP_LOGI(TAG, "Local wake ready: %s (%s), silence=%d ms",
             CONFIG_CMPP_VOICE_WAKE_WORD, CONFIG_CMPP_VOICE_WAKE_MODEL,
             CONFIG_CMPP_VOICE_WAKE_SILENCE_MS);
    return "ready";
failed:
    voice_wake_destroy(w);
    return status;
}

void voice_wake_set_barge_min_rms(voice_wake_t *w, unsigned rms)
{
    if (w) __atomic_store_n(&w->barge_min_rms, rms, __ATOMIC_RELAXED);
}

voice_wake_event_t voice_wake_step(voice_wake_t *w, voice_wake_mode_t mode,
                                  uint32_t generation, const int16_t *pcm)
{
    if (!w) return VOICE_WAKE_NONE;
    if (mode != w->mode || generation != w->generation || !pcm) {
        w->mode = mode;
        w->generation = generation;
        ++w->epoch;
        xQueueReset(w->frames);
        xQueueReset(w->results);
    }
    if (mode == VOICE_WAKE_OFF || !pcm) return VOICE_WAKE_NONE;
    wake_result_t result;
    if (xQueueReceive(w->results, &result, 0) == pdTRUE && result.epoch == w->epoch)
        return result.event;
    wake_frame_t frame = {.epoch = w->epoch, .mode = mode};
    memcpy(frame.pcm, pcm, sizeof(frame.pcm));
    if (xQueueSend(w->frames, &frame, 0) != pdTRUE) {
        ++w->epoch;
        xQueueReset(w->frames);
        xQueueReset(w->results);
        if (++w->dropped == 1 || w->dropped % 100 == 0)
            ESP_LOGW(TAG, "Recognition queue overruns=%lu; reset detector, PTT unaffected", (unsigned long)w->dropped);
    }
    return VOICE_WAKE_NONE;
}
