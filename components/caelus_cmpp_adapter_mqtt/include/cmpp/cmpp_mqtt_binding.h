/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>

#include "cmpp/cmpp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CMPP_MQTT_BINDING_VERSION "1.0"
#define CMPP_MQTT_DEFAULT_PREFIX "caelus/v1"
#define CMPP_MQTT_TOPIC_MAX 160U

typedef enum {
    CMPP_MQTT_TOPIC_COMMAND = 0,
    CMPP_MQTT_TOPIC_MANIFEST,
    CMPP_MQTT_TOPIC_STATE,
    CMPP_MQTT_TOPIC_AVAILABILITY,
    CMPP_MQTT_TOPIC_RESULT,
} cmpp_mqtt_topic_kind_t;

cmpp_status_t cmpp_mqtt_build_topic(
    char *output, size_t output_size, const char *prefix,
    const char *device_uid, cmpp_mqtt_topic_kind_t kind,
    const char *command_id);

cmpp_status_t cmpp_mqtt_dispatch_json(cmpp_runtime_t *runtime,
                                      const char *command_json,
                                      char **result_json);
void cmpp_mqtt_free_json(char *json);

#ifdef __cplusplus
}
#endif
