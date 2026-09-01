/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "cmpp/cmpp.h"
#include "esp_err.h"

typedef struct board_runtime board_runtime_t;

esp_err_t board_runtime_create(cmpp_runtime_t *runtime,
                               board_runtime_t **board);
esp_err_t board_runtime_safe_stop(board_runtime_t *board);
void board_runtime_destroy(board_runtime_t *board);

