/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_audio.h"
#include "voice_volume.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "voice_audio";
static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_input, s_output;
static bool s_rx_overflow;
#define VOICE_DMA_DESCRIPTORS 4
#define VOICE_DMA_FRAMES 160

static bool rx_overflow(i2s_chan_handle_t handle, i2s_event_data_t *event, void *context)
{
    (void)handle;
    (void)event;
    (void)context;
    __atomic_store_n(&s_rx_overflow, true, __ATOMIC_RELAXED);
    return false;
}

static esp_err_t codec_error(int err)
{
    return err == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t voice_audio_init(void)
{
    /* GPIO38 was latched by boot maintenance; leave it untouched here. */
    gpio_config_t power = {
        .pin_bit_mask = 1ULL << 47,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&power), TAG, "Power GPIO init failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(47, 1), TAG, "Analog power failed");
    gpio_config_t ptt = {
        .pin_bit_mask = 1ULL << 1, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&ptt), TAG, "PTT GPIO init failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t i2c = {
        .i2c_port = I2C_NUM_0, .sda_io_num = 11, .scl_io_num = 10,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c, &bus), TAG, "I2C init failed");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, ES7210_CODEC_DEFAULT_ADDR >> 1, 100),
                        TAG, "ES7210 not found at 0x40");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, ES8311_CODEC_DEFAULT_ADDR >> 1, 100),
                        TAG, "ES8311 not found at 0x18");

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = VOICE_DMA_DESCRIPTORS;
    channel.dma_frame_num = VOICE_DMA_FRAMES;
    channel.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &s_tx, &s_rx), TAG, "I2S allocation failed");
    /* Two physical mic inputs only. MIC3/AEC and wake models are outside this
     * transport bring-up. Equal stereo formats keep the shared clocks aligned. */
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = 12, .bclk = 13, .ws = 14, .dout = 16, .din = 15},
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &cfg), TAG, "I2S TX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &cfg), TAG, "I2S RX init failed");
    const i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = rx_overflow};
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_rx, &callbacks, NULL), TAG, "I2S overflow callback failed");
    audio_codec_i2s_cfg_t data_cfg = {.port = 0, .rx_handle = s_rx, .tx_handle = s_tx};
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&data_cfg);
    ESP_RETURN_ON_FALSE(data, ESP_ERR_NO_MEM, TAG, "I2S codec interface allocation failed");

    audio_codec_i2c_cfg_t ctrl_cfg = {.port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    const audio_codec_ctrl_if_t *out_ctrl = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(out_ctrl && gpio, ESP_ERR_NO_MEM, TAG, "DAC interface allocation failed");
    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = out_ctrl, .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = 21, .use_mclk = true, /* User confirmed MUTE -> NS4150 CTRL; high enables PA. */
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *dac = es8311_codec_new(&dac_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 init failed");
    esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data};
    s_output = esp_codec_dev_new(&dev_cfg);
    ctrl_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t *in_ctrl = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    ESP_RETURN_ON_FALSE(in_ctrl, ESP_ERR_NO_MEM, TAG, "ADC interface allocation failed");
    es7210_codec_cfg_t adc_cfg = {
        .ctrl_if = in_ctrl, .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *adc = es7210_codec_new(&adc_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7210 init failed");
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = adc;
    s_input = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_input && s_output, ESP_ERR_NO_MEM, TAG, "Codec allocation failed");
    esp_codec_dev_sample_info_t sample = {
        .bits_per_sample = 16, .channel = 2,
        .sample_rate = VOICE_SAMPLE_RATE, .mclk_multiple = 256,
    };
    ESP_RETURN_ON_ERROR(codec_error(esp_codec_dev_open(s_output, &sample)), TAG, "DAC open failed");
    ESP_RETURN_ON_ERROR(gpio_input_enable(21), TAG, "PA readback input failed");
    ESP_LOGI(TAG, "PA GPIO21 high requested; input readback=%d", gpio_get_level(21));
    ESP_RETURN_ON_ERROR(codec_error(esp_codec_dev_open(s_input, &sample)), TAG, "ADC open failed");
    ESP_RETURN_ON_ERROR(codec_error(esp_codec_dev_set_in_gain(s_input, CONFIG_CMPP_VOICE_INPUT_GAIN)), TAG, "ADC gain failed");
    return voice_volume_init(s_output);
}

esp_err_t voice_audio_read(int16_t pcm[VOICE_FRAME_SAMPLES])
{
    int16_t stereo[VOICE_FRAME_SAMPLES * 2];
    size_t bytes = 0;
    esp_err_t err = i2s_channel_read(s_rx, stereo, sizeof(stereo), &bytes, 100);
    if (err != ESP_OK || bytes != sizeof(stereo)) return err == ESP_OK ? ESP_FAIL : err;
    if (__atomic_exchange_n(&s_rx_overflow, false, __ATOMIC_RELAXED)) return ESP_ERR_INVALID_STATE;
    /* MIC1 is the bring-up source; validate its physical slot on hardware. */
    for (size_t i = 0; i < VOICE_FRAME_SAMPLES; ++i) pcm[i] = stereo[2 * i];
    return ESP_OK;
}

esp_err_t voice_audio_write(const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0 || samples > VOICE_FRAME_SAMPLES) return ESP_ERR_INVALID_ARG;
    int16_t stereo[VOICE_FRAME_SAMPLES * 2];
    for (size_t i = 0; i < samples; ++i) stereo[2 * i] = stereo[2 * i + 1] = pcm[i];
    size_t bytes = 0;
    esp_err_t err = i2s_channel_write(s_tx, stereo, samples * 4, &bytes, 100);
    return err != ESP_OK ? err : bytes == samples * 4 ? ESP_OK : ESP_FAIL;
}

esp_err_t voice_audio_mute(bool mute)
{
    return voice_volume_mute(mute);
}

esp_err_t voice_audio_discard_output(void)
{
    ESP_RETURN_ON_ERROR(voice_audio_mute(true), TAG, "Interrupt mute failed");
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx), TAG, "Interrupt TX stop failed");
    /* IDF disable resets the DMA cursor; preload overwrites every stereo slot.
     * Unlike writing 60 ms of silence, this doesn't block microphone capture
     * while the old TX ring drains. RX remains enabled throughout. */
    static const int16_t silence[VOICE_DMA_DESCRIPTORS * VOICE_DMA_FRAMES * 2] = {0};
    size_t loaded = 0;
    esp_err_t err = i2s_channel_preload_data(s_tx, silence, sizeof(silence), &loaded);
    esp_err_t enable_err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) return err;
    if (loaded != sizeof(silence)) return ESP_FAIL;
    return enable_err;
}

bool voice_audio_pressed(void)
{
    return gpio_get_level(1) == 0;
}
