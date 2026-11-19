/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_volume.h"
#include "nvs.h"
#include "voice_diagnostics.h"
#include <string.h>

static esp_codec_dev_handle_t s_output;
static int s_volume, s_saved_volume;
static bool s_turn_muted = true;
const uint8_t voice_codec_boot_registers[VOICE_CODEC_BOOT_REG_COUNT] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0d, 0x0e, 0x12, 0x31, 0x32, 0x37
};
static voice_codec_readback_t s_readback;

static void read_register(int reg, int *value, int *error)
{
    int observed = 0;
    *error = esp_codec_dev_read_reg(s_output, reg, &observed);
    *value = *error == ESP_CODEC_DEV_OK ? observed : -1;
    voice_diag_add(&s_readback.read_count, 1);
    if (*error != ESP_CODEC_DEV_OK) voice_diag_add(&s_readback.read_failed_count, 1);
}

static void read_mute(bool requested_mute)
{
    s_readback.requested_mute = requested_mute;
    read_register(0x31, &s_readback.mute_reg31, &s_readback.mute_error);
    if (!requested_mute) {
        s_readback.last_unmute_reg31 = s_readback.mute_reg31;
        s_readback.last_unmute_error = s_readback.mute_error;
        voice_diag_add(&s_readback.unmute_read_count, 1);
    }
}

static esp_err_t apply(int volume)
{
    if (esp_codec_dev_set_out_vol(s_output, volume) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    s_volume = volume;
    read_register(0x32, &s_readback.volume_reg32, &s_readback.volume_error);
    /* Explicit mute accompanies zero volume (the SDK maps zero to -96 dB).
     * Raising volume must not unmute an idle/closed channel. */
    int result = esp_codec_dev_set_out_mute(s_output, s_turn_muted || volume == 0);
    read_mute(s_turn_muted || volume == 0);
    return result == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t voice_volume_init(esp_codec_dev_handle_t output)
{
    if (!output) return ESP_ERR_INVALID_ARG;
    s_output = output;
    s_turn_muted = true;
    s_saved_volume = -1;
    memset(&s_readback, 0, sizeof(s_readback));
    s_readback.mute_reg31 = s_readback.volume_reg32 = s_readback.last_unmute_reg31 = -1;
    uint8_t saved = CONFIG_CMPP_VOICE_OUTPUT_VOLUME;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("mcp_light", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_u8(handle, "voice_volume", &saved);
        nvs_close(handle);
        if (err == ESP_OK) s_saved_volume = saved;
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (saved > 100) return ESP_ERR_INVALID_ARG;
    err = apply(saved);
    if (err != ESP_OK) return err;
    /* Once before capture/playback tasks exist. Reads are diagnostic only:
     * failed readback must not be mistaken for zero or mutate codec control. */
    for (int i = 0; i < VOICE_CODEC_BOOT_REG_COUNT; ++i)
        read_register(voice_codec_boot_registers[i], &s_readback.boot_values[i], &s_readback.boot_errors[i]);
    return ESP_OK;
}

esp_err_t voice_volume_set(int volume, bool *changed)
{
    if (!changed) return ESP_ERR_INVALID_ARG;
    *changed = false;
    if (volume < 0 || volume > 100) return ESP_ERR_INVALID_ARG;
    if (!s_output) return ESP_ERR_INVALID_STATE;
    bool different = volume != s_volume || volume != s_saved_volume;
    esp_err_t err = apply(volume);
    if (err != ESP_OK) return err;
    *changed = different;
    if (volume == s_saved_volume) return ESP_OK;
    nvs_handle_t handle;
    err = nvs_open("mcp_light", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "voice_volume", (uint8_t)volume);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    /* On storage failure the live value may have changed: caller reports
     * failed/effect unknown. Retrying the same value retries persistence. */
    if (err == ESP_OK) s_saved_volume = volume;
    return err;
}

esp_err_t voice_volume_mute(bool mute)
{
    if (!s_output) return ESP_ERR_INVALID_STATE;
    int result = esp_codec_dev_set_out_mute(s_output, mute || s_volume == 0);
    read_mute(mute || s_volume == 0);
    if (result != ESP_CODEC_DEV_OK) return ESP_FAIL;
    s_turn_muted = mute;
    return ESP_OK;
}

int voice_volume_get(void) { return s_volume; }

const voice_codec_readback_t *voice_volume_readback(void) { return &s_readback; }
