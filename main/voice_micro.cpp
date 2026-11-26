/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_micro.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "voice_micro_frontend.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

#ifdef CMPP_MICRO_MODEL_EMBEDDED
extern const uint8_t model_start[] asm("_binary_xiaobai_tflite_start");
extern const uint8_t model_end[] asm("_binary_xiaobai_tflite_end");
#endif
static constexpr size_t ARENA_BYTES = 256 * 1024;
static constexpr size_t VARIABLE_BYTES = 2048;
static const char *TAG = "voice_micro";
#ifdef CMPP_MICRO_SELF_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern const uint8_t positive_start[] asm("_binary_wake_positive_start");
extern const uint8_t positive_end[] asm("_binary_wake_positive_end");
extern const uint8_t negative_start[] asm("_binary_wake_negative_start");
extern const uint8_t negative_end[] asm("_binary_wake_negative_end");
static bool self_test(voice_micro_t *m)
{
    const uint8_t *begins[] = {negative_start, positive_start};
    const uint8_t *ends[] = {negative_end, positive_end};
    int64_t maximum_us = 0;
    unsigned frames = 0;
    for (int test = 0; test < 2; ++test) {
        if (!voice_micro_reset(m)) return false;
        bool detected = false;
        const auto *pcm = reinterpret_cast<const int16_t *>(begins[test]);
        size_t count = (ends[test] - begins[test]) / sizeof(int16_t);
        while (count) {
            size_t take = std::min(count, size_t(320));
            int64_t start = esp_timer_get_time();
            int result = voice_micro_process(m, pcm, take);
            maximum_us = std::max(maximum_us, esp_timer_get_time() - start);
            if (result < 0) return false;
            detected |= result > 0;
            pcm += take; count -= take;
            if (++frames % 8 == 0) vTaskDelay(1);
        }
        if (detected != (test == 1)) return false;
    }
    ESP_LOGI(TAG, "Embedded PCM self-test passed: positive=1 negative=0, max 20ms frame=%lld us", maximum_us);
    return voice_micro_reset(m);
}
#endif

struct voice_micro {
    tflite::MicroMutableOpResolver<12> ops;
    tflite::MicroInterpreter *interpreter = nullptr;
    uint8_t *arena = nullptr, *variables = nullptr;
    FrontendState frontend{};
    TfLiteTensor *input = nullptr, *output = nullptr;
    int slice = 0, windows = 0, hits = 0;
    bool healthy = true;
};

void voice_micro_destroy(voice_micro_t *m)
{
    if (!m) return;
    delete m->interpreter;
    FrontendFreeStateContents(&m->frontend);
    free(m->arena);
    free(m->variables);
    delete m;
}

bool voice_micro_reset(voice_micro_t *m)
{
    FrontendReset(&m->frontend);
    m->slice = m->windows = m->hits = 0;
    m->healthy = m->interpreter->Reset() == kTfLiteOk;
    return m->healthy;
}

const char *voice_micro_create(voice_micro_t **out)
{
    *out = nullptr;
#ifndef CMPP_MICRO_MODEL_EMBEDDED
    return "model_missing_or_invalid";
#else
    if (strcmp(CONFIG_CMPP_VOICE_WAKE_WORD, "小白小白") ||
        strcmp(CONFIG_CMPP_VOICE_WAKE_MODEL, "xiaobai_micro_v1")) return "word_mismatch";
    flatbuffers::Verifier verifier(model_start, model_end - model_start);
    if (!tflite::VerifyModelBuffer(verifier)) return "model_missing_or_invalid";
    auto model = tflite::GetModel(model_start);
    if (model->version() != TFLITE_SCHEMA_VERSION) return "unsupported_model";
    auto m = new (std::nothrow) voice_micro;
    if (!m) return "no_memory";
    auto fail = [m](const char *status) { voice_micro_destroy(m); return status; };
    m->arena = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    m->variables = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, VARIABLE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!m->arena || !m->variables) return fail("no_memory");
#define ADD_OP(name) if (m->ops.Add##name() != kTfLiteOk) return fail("unsupported_model")
    ADD_OP(CallOnce); ADD_OP(VarHandle); ADD_OP(Reshape); ADD_OP(ReadVariable);
    ADD_OP(StridedSlice); ADD_OP(Concatenation); ADD_OP(AssignVariable); ADD_OP(Conv2D);
    ADD_OP(FullyConnected); ADD_OP(Logistic); ADD_OP(Quantize); ADD_OP(DepthwiseConv2D);
#undef ADD_OP
    auto allocator = tflite::MicroAllocator::Create(m->variables, VARIABLE_BYTES);
    if (!allocator) return fail("no_memory");
    auto variables = tflite::MicroResourceVariables::Create(allocator, 20);
    if (!variables) return fail("no_memory");
    m->interpreter = new (std::nothrow) tflite::MicroInterpreter(model, m->ops, m->arena, ARENA_BYTES, variables);
    if (!m->interpreter || m->interpreter->AllocateTensors() != kTfLiteOk) return fail("model_init_failed");
    m->input = m->interpreter->input(0);
    m->output = m->interpreter->output(0);
    if (m->input->type != kTfLiteInt8 || m->input->dims->size != 3 ||
        m->input->dims->data[0] != 1 || m->input->dims->data[1] != 3 || m->input->dims->data[2] != 40 ||
        m->output->type != kTfLiteUInt8 || m->output->bytes != 1 ||
        !std::isfinite(m->input->params.scale) || m->input->params.scale <= 0 ||
        !std::isfinite(m->output->params.scale) || m->output->params.scale <= 0)
        return fail("unsupported_model");
    auto config = voice_micro_frontend_config();
    if (!FrontendPopulateState(&config, &m->frontend, 16000)) return fail("no_memory");
    if (!voice_micro_reset(m)) return fail("model_init_failed");
#ifdef CMPP_MICRO_SELF_TEST
    if (!self_test(m)) return fail("model_selftest_failed");
#endif
    ESP_LOGI(TAG, "xiaobai_micro_v1: model=%u arena=%u input_scale=%.7f zero=%d",
             unsigned(model_end - model_start), unsigned(m->interpreter->arena_used_bytes()),
             double(m->input->params.scale), m->input->params.zero_point);
    *out = m;
    return "ready";
#endif
}

int voice_micro_process(voice_micro_t *m, const int16_t *pcm, size_t count)
{
    if (!m->healthy) return -1;
    bool detected = false;
    while (count) {
        size_t used = 0;
        auto f = FrontendProcessSamples(&m->frontend, pcm, count, &used);
        if (!used || used > count) { m->healthy = false; return -1; }
        pcm += used; count -= used;
        if (!f.size) continue;
        if (f.size != 40) { m->healthy = false; return -1; }
        for (size_t i = 0; i < f.size; ++i) {
            // pymicro-features outputs raw frontend values / 25.6.
            int value = std::lround((f.values[i] / 25.6f) / m->input->params.scale) + m->input->params.zero_point;
            m->input->data.int8[m->slice * 40 + i] = std::clamp(value, -128, 127);
        }
        if (++m->slice != 3) continue;
        m->slice = 0;
        auto start = esp_timer_get_time();
        if (m->interpreter->Invoke() != kTfLiteOk) { m->healthy = false; return -1; }
        float probability = (m->output->data.uint8[0] - m->output->params.zero_point) * m->output->params.scale;
        float threshold = CONFIG_CMPP_VOICE_WAKE_THRESHOLD > 0 ? CONFIG_CMPP_VOICE_WAKE_THRESHOLD / 100.0f : 0.90f;
        ++m->windows;
        // Validation-selected gate: nine 30 ms predictions (270 ms).
        m->hits = (m->windows >= 34 && probability >= threshold) ? m->hits + 1 : 0;
        if (m->hits == 9) {
            ESP_LOGI(TAG, "Detected 小白小白 probability=%.3f inference=%lld us", double(probability), esp_timer_get_time() - start);
            detected = true;
        }
    }
    return detected ? 1 : 0;
}
