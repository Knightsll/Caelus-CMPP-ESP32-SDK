/* SPDX-License-Identifier: Apache-2.0 */
#include "mqtt_settings.h"

#include <stdbool.h>
#include <string.h>

#include "cmpp/cmpp_mqtt_binding.h"
#include "nvs.h"

#define MQTT_NAMESPACE "cmpp_mqtt"
#define MQTT_SETTINGS_KEY "settings"

static bool valid_uri(const char *uri)
{
    const char *authority = NULL;
    if (uri && strncmp(uri, "mqtt://", 7U) == 0) {
        authority = uri + 7U;
    } else if (uri && strncmp(uri, "mqtts://", 8U) == 0) {
        authority = uri + 8U;
    } else if (uri && strncmp(uri, "ws://", 5U) == 0) {
        authority = uri + 5U;
    } else if (uri && strncmp(uri, "wss://", 6U) == 0) {
        authority = uri + 6U;
    }
    if (!authority || authority[0] == '\0' || authority[0] == '/' ||
        authority[0] == '?' || authority[0] == '#') {
        return false;
    }
    const size_t authority_len = strcspn(authority, "/?#");
    for (size_t index = 0; index < authority_len; ++index) {
        const unsigned char value = (unsigned char)authority[index];
        if (value <= 0x20U || value == 0x7fU || value == '@') {
            return false;
        }
    }
    return true;
}

bool mqtt_settings_valid(const mqtt_settings_t *settings)
{
    if (!settings || !valid_uri(settings->broker_uri) ||
        settings->topic_prefix[0] == '\0') {
        return false;
    }
    char topic[CMPP_MQTT_TOPIC_MAX];
    return cmpp_mqtt_build_topic(topic, sizeof(topic), settings->topic_prefix,
                                 "device-validation",
                                 CMPP_MQTT_TOPIC_COMMAND, NULL) == CMPP_OK;
}

esp_err_t mqtt_settings_load(mqtt_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(settings, 0, sizeof(*settings));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MQTT_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t size = sizeof(*settings);
    err = nvs_get_blob(handle, MQTT_SETTINGS_KEY, settings, &size);
    nvs_close(handle);
    if (err == ESP_OK &&
        (size != sizeof(*settings) || !mqtt_settings_valid(settings))) {
        memset(settings, 0, sizeof(*settings));
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

esp_err_t mqtt_settings_save(const mqtt_settings_t *settings)
{
    if (!mqtt_settings_valid(settings)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MQTT_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, MQTT_SETTINGS_KEY, settings,
                       sizeof(*settings));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
