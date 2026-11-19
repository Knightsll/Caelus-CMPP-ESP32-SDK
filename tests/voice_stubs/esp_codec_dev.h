#pragma once
#include <stdbool.h>
typedef void *esp_codec_dev_handle_t;
#define ESP_CODEC_DEV_OK 0
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t handle, int volume);
int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t handle, bool mute);

int esp_codec_dev_read_reg(esp_codec_dev_handle_t handle, int reg, int *value);
