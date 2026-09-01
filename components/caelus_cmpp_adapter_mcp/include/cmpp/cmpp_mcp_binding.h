/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "cmpp/cmpp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CMPP_MCP_TOOL_EXECUTE "cmpp.execute"
#define CMPP_MCP_RESOURCE_MANIFEST "cmpp://manifest"
#define CMPP_MCP_RESOURCE_STATE "cmpp://state"
#define CMPP_MCP_RESOURCE_HEALTH "cmpp://health"
#define CMPP_MCP_RESOURCE_LIMITATIONS "cmpp://limitations"
#define CMPP_MCP_RESOURCE_LATEST_RESULT "cmpp://commands/latest-result"
#define CMPP_MCP_RESOURCE_LATEST_EVENT "cmpp://events/latest"

cmpp_status_t cmpp_mcp_dispatch_json(cmpp_runtime_t *runtime,
                                     const char *command_json,
                                     char **result_json);
void cmpp_mcp_free_json(char *json);

const char *cmpp_mcp_protocol_version(void);
const char *cmpp_mcp_discovery_service(void);

#ifdef __cplusplus
}
#endif

