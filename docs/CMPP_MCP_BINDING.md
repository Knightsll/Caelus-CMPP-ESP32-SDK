# CMPP MCP Binding 1.0

This binding is implemented against MCP protocol `2025-11-25` using
`espressif/mcp-c-sdk` 2.0.1. MCP-version constants and MCP types exist only in
`caelus_cmpp_adapter_mcp`.

## Authentication and discovery

`cmpp_mcp_adapter_create()` refuses to attach unless the caller confirms that
the MCP transport enforces authentication. Authentication happens before the
tool callback; credentials are never part of a CMPP command. A device may
advertise `_cmpp._tcp` and should include endpoint, stable device ID,
authentication mode and pairing state in discovery metadata.

## Tool

The adapter registers `cmpp.execute`. Its single `command` property is a JSON
object conforming to `cmpp.command.v1`:

```json
{
  "schema": "cmpp.command.v1",
  "protocol_version": "1.0",
  "command_id": "cmd-001",
  "trace_id": "trace-001",
  "target": {
    "device_id": "device-001",
    "capability_id": "fan.speed"
  },
  "command_type": "fan.speed.set",
  "requested_by": "operator-console",
  "reason": "Set ventilation level",
  "safety_class": "reversible",
  "arguments": {"output": 4000},
  "ttl_ms": 5000,
  "idempotency_key": "ventilation-001",
  "dry_run": false,
  "requires_ack": true
}
```

`target` deliberately has no MCP server, session or adapter identity. An
optional `absolute_deadline_ms` is measured in the device monotonic clock and is
therefore suitable only when the caller shares that clock domain; normal remote
clients use `ttl_ms`.

The tool returns identical JSON in a text content block and MCP
`structuredContent`. The object uses schema `cmpp.command-result.v1` and carries
`command_id`, `trace_id`, `status`, `effect`, `state_version`,
`idempotent_replay`, optional `result`, and optional `error`. Rejected and failed
results set MCP `isError`; they remain valid CMPP receipts rather than JSON-RPC
transport errors.

Malformed MCP input that cannot become a CMPP request returns
`cmpp.binding-error.v1`.

## Resources

| URI | Content |
|---|---|
| `cmpp://manifest` | Registered descriptors and schemas |
| `cmpp://state` | State version and current observations |
| `cmpp://health` | Runtime health summary |
| `cmpp://limitations` | Idempotency, serialization, feedback and auth limits |
| `cmpp://commands/latest-result` | Latest synchronous command result |
| `cmpp://events/latest` | Latest separately published event |

Capability enumeration is descriptor-driven. Adding a Function Module requires
no adapter branch.

## Events

The adapter stores the latest event separately and emits it through MCP
`notifications/message` using logger `cmpp`. Event publication is best effort;
the authoritative synchronous receipt remains the tool result. MCP notification
failure does not change the hardware effect or receipt.

