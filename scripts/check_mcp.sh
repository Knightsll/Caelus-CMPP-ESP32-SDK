#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: bash scripts/check_mcp.sh URL BEARER_TOKEN" >&2
    exit 2
fi

endpoint="$1"
token="$2"
headers_file="$(mktemp /tmp/caelus-mcp-headers.XXXXXX)"
body_file="$(mktemp /tmp/caelus-mcp-body.XXXXXX)"
trap 'rm -f "$headers_file" "$body_file"' EXIT

curl --noproxy '*' --fail-with-body --silent --show-error \
    --connect-timeout 5 --max-time 15 \
    --dump-header "$headers_file" --output "$body_file" \
    --request POST "$endpoint" \
    --header "Authorization: Bearer $token" \
    --header 'Content-Type: application/json' \
    --header 'Accept: application/json, text/event-stream' \
    --data '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"caelus-mcp-check","version":"1.0.0"}}}'

session_id="$(awk -F ': *' 'tolower($1) == "mcp-session-id" {gsub("\r", "", $2); print $2}' "$headers_file" | tail -n 1)"
if [[ -z "$session_id" ]]; then
    echo "MCP initialize response did not include MCP-Session-Id" >&2
    cat "$body_file" >&2
    exit 1
fi

echo "MCP initialize OK"
cat "$body_file"
echo

curl --noproxy '*' --fail-with-body --silent --show-error \
    --connect-timeout 5 --max-time 15 --output /dev/null \
    --request POST "$endpoint" \
    --header "Authorization: Bearer $token" \
    --header "MCP-Session-Id: $session_id" \
    --header 'MCP-Protocol-Version: 2025-11-25' \
    --header 'Content-Type: application/json' \
    --header 'Accept: application/json, text/event-stream' \
    --data '{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}'

curl --noproxy '*' --fail-with-body --silent --show-error \
    --connect-timeout 5 --max-time 15 \
    --request POST "$endpoint" \
    --header "Authorization: Bearer $token" \
    --header "MCP-Session-Id: $session_id" \
    --header 'MCP-Protocol-Version: 2025-11-25' \
    --header 'Content-Type: application/json' \
    --header 'Accept: application/json, text/event-stream' \
    --data '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
echo
echo "MCP authentication and tools/list OK"

curl --noproxy '*' --fail-with-body --silent --show-error \
    --connect-timeout 5 --max-time 15 \
    --request POST "$endpoint" \
    --header "Authorization: Bearer $token" \
    --header "MCP-Session-Id: $session_id" \
    --header 'MCP-Protocol-Version: 2025-11-25' \
    --header 'Content-Type: application/json' \
    --header 'Accept: application/json, text/event-stream' \
    --data '{"jsonrpc":"2.0","id":3,"method":"resources/list","params":{}}'
echo
echo "MCP resources/list OK"
