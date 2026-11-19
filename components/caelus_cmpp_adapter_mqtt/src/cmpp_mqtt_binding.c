/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/cmpp_mqtt_binding.h"

#include <stdio.h>
#include <string.h>

#include "cmpp/cmpp_codec_json.h"

static bool topic_segment_valid(const char *value)
{
    if (!value || value[0] == '\0') {
        return false;
    }
    for (const unsigned char *item = (const unsigned char *)value;
         *item != '\0'; ++item) {
        if (*item <= 0x20U || *item == 0x7fU || *item == '+' ||
            *item == '#') {
            return false;
        }
    }
    return true;
}

cmpp_status_t cmpp_mqtt_build_topic(
    char *output, size_t output_size, const char *prefix,
    const char *device_uid, cmpp_mqtt_topic_kind_t kind,
    const char *command_id)
{
    if (!output || output_size == 0U || !topic_segment_valid(prefix) ||
        prefix[0] == '/' || prefix[strlen(prefix) - 1U] == '/' ||
        !topic_segment_valid(device_uid) || strchr(device_uid, '/')) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }

    const char *suffix = NULL;
    switch (kind) {
    case CMPP_MQTT_TOPIC_COMMAND:
        suffix = "command";
        break;
    case CMPP_MQTT_TOPIC_MANIFEST:
        suffix = "manifest";
        break;
    case CMPP_MQTT_TOPIC_STATE:
        suffix = "state";
        break;
    case CMPP_MQTT_TOPIC_AVAILABILITY:
        suffix = "availability";
        break;
    case CMPP_MQTT_TOPIC_RESULT:
        if (!topic_segment_valid(command_id) || strchr(command_id, '/')) {
            return CMPP_ERR_INVALID_ARGUMENT;
        }
        break;
    default:
        return CMPP_ERR_INVALID_ARGUMENT;
    }

    const int written = kind == CMPP_MQTT_TOPIC_RESULT
                            ? snprintf(output, output_size, "%s/%s/result/%s",
                                       prefix, device_uid, command_id)
                            : snprintf(output, output_size, "%s/%s/%s",
                                       prefix, device_uid, suffix);
    return written >= 0 && (size_t)written < output_size
               ? CMPP_OK
               : CMPP_ERR_INVALID_ARGUMENT;
}

cmpp_status_t cmpp_mqtt_dispatch_json(cmpp_runtime_t *runtime,
                                      const char *command_json,
                                      char **result_json)
{
    if (!runtime || !command_json || !result_json) {
        return CMPP_ERR_INVALID_ARGUMENT;
    }
    *result_json = NULL;

    cmpp_json_document_t *document = NULL;
    cmpp_request_t request;
    cmpp_status_t status = cmpp_json_decode_request(command_json, &document,
                                                     &request);
    if (status != CMPP_OK) {
        return status;
    }

    cmpp_response_t response;
    status = cmpp_runtime_dispatch(runtime, &request, &response);
    if (status == CMPP_OK) {
        status = cmpp_json_encode_response(&response, result_json);
        cmpp_response_deinit(runtime, &response);
    }
    cmpp_json_document_destroy(document);
    return status;
}

void cmpp_mqtt_free_json(char *json)
{
    cmpp_json_free(json);
}
