#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define MQTT_BROKER_URI_MAX_LEN 192
#define MQTT_USERNAME_MAX_LEN 64
#define MQTT_PASSWORD_MAX_LEN 128
#define MQTT_TOPIC_PREFIX_MAX_LEN 64

typedef struct {
    char broker_uri[MQTT_BROKER_URI_MAX_LEN];
    char username[MQTT_USERNAME_MAX_LEN];
    char password[MQTT_PASSWORD_MAX_LEN];
    char topic_prefix[MQTT_TOPIC_PREFIX_MAX_LEN];
} mqtt_settings_t;

bool mqtt_settings_valid(const mqtt_settings_t *settings);
esp_err_t mqtt_settings_load(mqtt_settings_t *settings);
esp_err_t mqtt_settings_save(const mqtt_settings_t *settings);
