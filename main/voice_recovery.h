/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_err.h"

/* Run after NVS initialization, before identity, audio or networking services. */
esp_err_t voice_recovery_boot(void);

/* Start after boot recovery and LED init, before the blocking network setup. */
esp_err_t voice_recovery_start(void);
