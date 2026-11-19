#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t network_connect(void);
esp_err_t network_get_ip(char *buffer, size_t buffer_size);
int network_get_rssi(void);
bool network_is_connected(void);
