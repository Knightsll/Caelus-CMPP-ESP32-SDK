/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_codec_dev.h"

/* Serialized by the media lock after initialization. Values are configuration,
 * not register readback: the official codec API may mask I2C write errors. */
esp_err_t voice_volume_init(esp_codec_dev_handle_t output);
esp_err_t voice_volume_set(int volume, bool *changed);
esp_err_t voice_volume_mute(bool mute);
int voice_volume_get(void);

#define VOICE_CODEC_BOOT_REG_COUNT 16
extern const uint8_t voice_codec_boot_registers[VOICE_CODEC_BOOT_REG_COUNT];
typedef struct {
    int boot_values[VOICE_CODEC_BOOT_REG_COUNT], boot_errors[VOICE_CODEC_BOOT_REG_COUNT];
    int mute_reg31, mute_error, volume_reg32, volume_error;
    int last_unmute_reg31, last_unmute_error;
    bool requested_mute;
    uint32_t read_count, read_failed_count, unmute_read_count;
} voice_codec_readback_t;
/* Cached fixed-register diagnostics only; getter performs no I2C transactions. */
const voice_codec_readback_t *voice_volume_readback(void);
