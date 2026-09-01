/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "cmpp/cmpp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cmpp_json_document cmpp_json_document_t;

cmpp_status_t cmpp_json_parse_value(const char *json,
                                    cmpp_json_document_t **document,
                                    const cmpp_value_t **value);
cmpp_status_t cmpp_json_decode_request(const char *json,
                                       cmpp_json_document_t **document,
                                       cmpp_request_t *request);
void cmpp_json_document_destroy(cmpp_json_document_t *document);

cmpp_status_t cmpp_json_encode_value(const cmpp_value_t *value, char **json);
cmpp_status_t cmpp_json_encode_response(const cmpp_response_t *response,
                                        char **json);
cmpp_status_t cmpp_json_encode_manifest(cmpp_runtime_t *runtime, char **json);
cmpp_status_t cmpp_json_encode_health(cmpp_runtime_t *runtime, char **json);
void cmpp_json_free(char *json);

#ifdef __cplusplus
}
#endif

