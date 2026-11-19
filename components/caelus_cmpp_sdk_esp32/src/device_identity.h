#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define DEVICE_UID_MAX_LEN 32
#define DEVICE_HOSTNAME_MAX_LEN 64
#define DEVICE_DISPLAY_NAME_MAX_LEN 64
#define DEVICE_AUTH_TOKEN_BYTES 32
#define DEVICE_AUTH_TOKEN_MAX_LEN ((DEVICE_AUTH_TOKEN_BYTES * 2) + 1)

typedef struct {
    char device_uid[DEVICE_UID_MAX_LEN];
    char hostname[DEVICE_HOSTNAME_MAX_LEN];
    char display_name[DEVICE_DISPLAY_NAME_MAX_LEN];
    char auth_token[DEVICE_AUTH_TOKEN_MAX_LEN];
} device_identity_t;

esp_err_t device_identity_load(device_identity_t *identity);
esp_err_t device_identity_set_setup(const char *display_name,
                                    const char *hostname);
esp_err_t device_identity_complete_pairing(void);
esp_err_t device_identity_pairing_complete(bool *complete);
