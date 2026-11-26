/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "frontend_util.h"
static inline struct FrontendConfig voice_micro_frontend_config(void)
{
    struct FrontendConfig config;
    FrontendFillConfigWithDefaults(&config);
    config.window.size_ms = 30;
    config.window.step_size_ms = 10;
    config.filterbank.num_channels = 40;
    config.filterbank.lower_band_limit = 125;
    config.filterbank.upper_band_limit = 7500;
    config.pcan_gain_control.enable_pcan = 1;
    return config;
}
