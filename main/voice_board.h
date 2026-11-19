/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "board_config.h"
#include "device_identity.h"

esp_err_t voice_board_start(board_runtime_t *board, const device_identity_t *identity);
