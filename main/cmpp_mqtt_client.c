/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp_mqtt_client.h"

#include "sdkconfig.h"

#if CONFIG_CMPP_BOARD_MQTT_ENABLE

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cmpp/cmpp_codec_json.h"
#include "cmpp/cmpp_mqtt_binding.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "mqtt_settings.h"
#include "nvs.h"

#define MQTT_COMMAND_MAX_BYTES 4096U
#define MQTT_COMMAND_QUEUE_LENGTH 4U

typedef struct {
    esp_mqtt_client_handle_t handle;
    cmpp_runtime_t *runtime;
    device_identity_t identity;
    mqtt_settings_t settings;
    QueueHandle_t command_queue;
    char command_topic[CMPP_MQTT_TOPIC_MAX];
    char manifest_topic[CMPP_MQTT_TOPIC_MAX];
    char state_topic[CMPP_MQTT_TOPIC_MAX];
    char availability_topic[CMPP_MQTT_TOPIC_MAX];
    char *fragment;
    size_t fragment_size;
    int fragment_message_id;
} mqtt_client_state_t;

static const char *TAG = "cmpp_mqtt";
static mqtt_client_state_t s_client;

static bool secure_uri(const char *uri)
{
    return strncmp(uri, "mqtts://", 8U) == 0 ||
           strncmp(uri, "wss://", 6U) == 0;
}

static void clear_fragment(void)
{
    free(s_client.fragment);
    s_client.fragment = NULL;
    s_client.fragment_size = 0U;
    s_client.fragment_message_id = 0;
}

static void publish_json(const char *topic, const char *json, bool retain)
{
    const int result = esp_mqtt_client_enqueue(
        s_client.handle, topic, json, 0, 1, retain ? 1 : 0, true);
    if (result < 0) {
        ESP_LOGW(TAG, "MQTT outbox rejected publish on %s", topic);
    }
}

static void publish_manifest(void)
{
    char *manifest = NULL;
    if (cmpp_json_encode_manifest(s_client.runtime, &manifest) == CMPP_OK) {
        publish_json(s_client.manifest_topic, manifest, true);
        cmpp_json_free(manifest);
    } else {
        ESP_LOGW(TAG, "Could not encode MQTT manifest");
    }
}

static void publish_state(void)
{
    cmpp_runtime_info_t info;
    if (cmpp_runtime_get_info(s_client.runtime, &info) != CMPP_OK) {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *states = root ? cJSON_AddObjectToObject(root, "capabilities") : NULL;
    if (!root || !states) {
        cJSON_Delete(root);
        return;
    }
    cJSON_AddStringToObject(root, "schema", "cmpp.state.v1");
    cJSON_AddStringToObject(root, "device_id", info.device_id);
    cJSON_AddNumberToObject(root, "state_version", (double)info.state_version);
    for (size_t index = 0; index < info.capability_count; ++index) {
        const cmpp_capability_descriptor_t *descriptor = NULL;
        cmpp_value_t value;
        if (cmpp_runtime_get_capability(s_client.runtime, index, &descriptor) !=
                CMPP_OK ||
            cmpp_runtime_observe(s_client.runtime, descriptor->capability_id,
                                 &value) != CMPP_OK) {
            continue;
        }
        char *value_json = NULL;
        if (cmpp_json_encode_value(&value, &value_json) == CMPP_OK) {
            cJSON *item = cJSON_Parse(value_json);
            if (item) {
                cJSON_AddItemToObject(states, descriptor->capability_id, item);
            }
            cmpp_json_free(value_json);
        }
        cmpp_value_deinit(s_client.runtime, &value);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        publish_json(s_client.state_topic, json, true);
        cJSON_free(json);
    }
}

static const char *result_command_id(const char *json)
{
    static char command_id[CMPP_ID_MAX + 1U];
    command_id[0] = '\0';
    cJSON *root = cJSON_Parse(json);
    const cJSON *item = root
                            ? cJSON_GetObjectItemCaseSensitive(root,
                                                               "command_id")
                            : NULL;
    if (cJSON_IsString(item) && item->valuestring &&
        strlen(item->valuestring) <= CMPP_ID_MAX) {
        strlcpy(command_id, item->valuestring, sizeof(command_id));
    }
    cJSON_Delete(root);
    return command_id[0] ? command_id : "_binding";
}

static void command_worker(void *context)
{
    (void)context;
    char *command = NULL;
    while (xQueueReceive(s_client.command_queue, &command, portMAX_DELAY) ==
           pdTRUE) {
        char *result = NULL;
        const cmpp_status_t status = cmpp_mqtt_dispatch_json(
            s_client.runtime, command, &result);
        free(command);
        command = NULL;

        char binding_error[160];
        if (status != CMPP_OK) {
            (void)snprintf(binding_error, sizeof(binding_error),
                           "{\"schema\":\"cmpp.binding-error.v1\","
                           "\"code\":\"%s\"}",
                           cmpp_status_string(status));
            result = binding_error;
        }

        char topic[CMPP_MQTT_TOPIC_MAX];
        if (cmpp_mqtt_build_topic(
                topic, sizeof(topic), s_client.settings.topic_prefix,
                s_client.identity.device_uid, CMPP_MQTT_TOPIC_RESULT,
                result_command_id(result)) == CMPP_OK) {
            publish_json(topic, result, false);
        }
        if (status == CMPP_OK) {
            publish_state();
        }
        if (status == CMPP_OK) {
            cmpp_mqtt_free_json(result);
        }
    }
    vTaskDelete(NULL);
}

static void receive_command(esp_mqtt_event_handle_t event)
{
    if (event->retain || event->total_data_len <= 0 ||
        (size_t)event->total_data_len > MQTT_COMMAND_MAX_BYTES ||
        event->current_data_offset < 0 || event->data_len < 0 ||
        event->current_data_offset + event->data_len > event->total_data_len) {
        clear_fragment();
        ESP_LOGW(TAG, "Rejected oversized or malformed MQTT command");
        return;
    }

    if (event->current_data_offset == 0) {
        clear_fragment();
        if (!event->topic ||
            strlen(s_client.command_topic) != (size_t)event->topic_len ||
            memcmp(event->topic, s_client.command_topic,
                   (size_t)event->topic_len) != 0) {
            return;
        }
        s_client.fragment = calloc((size_t)event->total_data_len + 1U, 1U);
        if (!s_client.fragment) {
            ESP_LOGW(TAG, "No memory for MQTT command");
            return;
        }
        s_client.fragment_size = (size_t)event->total_data_len;
        s_client.fragment_message_id = event->msg_id;
    }

    if (!s_client.fragment || event->msg_id != s_client.fragment_message_id ||
        (size_t)event->total_data_len != s_client.fragment_size) {
        clear_fragment();
        ESP_LOGW(TAG, "Rejected inconsistent MQTT command fragments");
        return;
    }
    memcpy(s_client.fragment + event->current_data_offset, event->data,
           (size_t)event->data_len);

    if ((size_t)(event->current_data_offset + event->data_len) ==
        s_client.fragment_size) {
        char *complete = s_client.fragment;
        s_client.fragment = NULL;
        s_client.fragment_size = 0U;
        s_client.fragment_message_id = 0;
        if (xQueueSend(s_client.command_queue, &complete, 0) != pdTRUE) {
            free(complete);
            ESP_LOGW(TAG, "MQTT command queue is full");
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        if (esp_mqtt_client_subscribe(s_client.handle,
                                      s_client.command_topic, 1) < 0) {
            ESP_LOGE(TAG, "Could not subscribe to CMPP MQTT commands");
            return;
        }
        publish_json(s_client.availability_topic, "online", true);
        publish_manifest();
        publish_state();
        ESP_LOGI(TAG, "CMPP MQTT connected; command topic: %s",
                 s_client.command_topic);
    } else if (event_id == MQTT_EVENT_DATA) {
        receive_command(event);
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        clear_fragment();
        ESP_LOGW(TAG, "MQTT disconnected; automatic reconnect enabled");
    } else if (event_id == MQTT_EVENT_ERROR) {
        ESP_LOGW(TAG, "MQTT transport error");
    }
}

esp_err_t cmpp_mqtt_client_start(const device_identity_t *identity,
                                 cmpp_runtime_t *runtime)
{
    if (!identity || !runtime || s_client.handle) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = mqtt_settings_load(&s_client.settings);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "MQTT is enabled but no broker was provisioned");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    s_client.runtime = runtime;
    s_client.identity = *identity;
    if (cmpp_mqtt_build_topic(
            s_client.command_topic, sizeof(s_client.command_topic),
            s_client.settings.topic_prefix, identity->device_uid,
            CMPP_MQTT_TOPIC_COMMAND, NULL) != CMPP_OK ||
        cmpp_mqtt_build_topic(
            s_client.manifest_topic, sizeof(s_client.manifest_topic),
            s_client.settings.topic_prefix, identity->device_uid,
            CMPP_MQTT_TOPIC_MANIFEST, NULL) != CMPP_OK ||
        cmpp_mqtt_build_topic(
            s_client.state_topic, sizeof(s_client.state_topic),
            s_client.settings.topic_prefix, identity->device_uid,
            CMPP_MQTT_TOPIC_STATE, NULL) != CMPP_OK ||
        cmpp_mqtt_build_topic(
            s_client.availability_topic, sizeof(s_client.availability_topic),
            s_client.settings.topic_prefix, identity->device_uid,
            CMPP_MQTT_TOPIC_AVAILABILITY, NULL) != CMPP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    s_client.command_queue = xQueueCreate(MQTT_COMMAND_QUEUE_LENGTH,
                                           sizeof(char *));
    if (!s_client.command_queue ||
        xTaskCreate(command_worker, "cmpp_mqtt_cmd", 6144, NULL, 5, NULL) !=
            pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const bool tls = secure_uri(s_client.settings.broker_uri);
    const esp_mqtt_client_config_t config = {
        .broker.address.uri = s_client.settings.broker_uri,
        .broker.verification.crt_bundle_attach =
            tls ? esp_crt_bundle_attach : NULL,
        .credentials.username = s_client.settings.username[0]
                                    ? s_client.settings.username
                                    : NULL,
        .credentials.client_id = s_client.identity.device_uid,
        .credentials.authentication.password =
            s_client.settings.password[0] ? s_client.settings.password : NULL,
        .session.last_will.topic = s_client.availability_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
        .task.stack_size = 5120,
        .buffer.size = 2048,
        .buffer.out_size = 2048,
        .outbox.limit = 16384,
    };
    s_client.handle = esp_mqtt_client_init(&config);
    if (!s_client.handle) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(
                            s_client.handle, ESP_EVENT_ANY_ID,
                            mqtt_event_handler, NULL),
                        TAG, "MQTT event registration failed");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(s_client.handle), TAG,
                        "MQTT client start failed");
    ESP_LOGI(TAG, "Starting optional CMPP MQTT transport (%s)",
             tls ? "TLS" : "plaintext LAN");
    return ESP_OK;
}

#else

esp_err_t cmpp_mqtt_client_start(const device_identity_t *identity,
                                 cmpp_runtime_t *runtime)
{
    (void)identity;
    (void)runtime;
    return ESP_OK;
}

#endif
