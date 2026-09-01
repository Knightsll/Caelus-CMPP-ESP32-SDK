/* SPDX-License-Identifier: Apache-2.0 */
#include "cmpp/cmpp_mcp_binding.h"

#include "cmpp/cmpp_codec_json.h"

#define MCP_BINDING_VERSION "2025-11-25"

cmpp_status_t cmpp_mcp_dispatch_json(cmpp_runtime_t *runtime,
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

void cmpp_mcp_free_json(char *json)
{
    cmpp_json_free(json);
}

const char *cmpp_mcp_protocol_version(void)
{
    return MCP_BINDING_VERSION;
}

const char *cmpp_mcp_discovery_service(void)
{
    return "_cmpp._tcp";
}
