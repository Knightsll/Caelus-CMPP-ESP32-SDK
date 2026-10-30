# CMPP Transport Adapter Guide

A transport adapter converts its protocol envelope into `cmpp_request_t`, calls
`cmpp_runtime_dispatch()`, and converts `cmpp_response_t` back to the transport.
It must not implement capability dispatch, safety decisions, deadlines,
idempotency, state versioning or hardware operations.

## Required mapping

1. Authenticate before accepting command content.
2. Record command receipt time as close to transport ingress as possible.
3. Decode all required CMPP fields without silently coercing types.
4. Dispatch once through the public Core API.
5. Preserve `status`, `effect`, error information, state version and replay flag.
6. Release the response using `cmpp_response_deinit()`.
7. Expose manifest, state, health and limitations without product-specific
   branches.
8. Carry events separately from synchronous command results.

Protocol negotiation, sessions, transport authentication types and wire-format
objects stay in the adapter component. The adapter may use
`cmpp_codec_json`, but Core and Function Modules must not.

The in-process path is the reference non-transport adapter: build a
`cmpp_request_t` and call `cmpp_runtime_dispatch()` directly. Contract tests
should issue semantically equivalent commands through this path and through the
new adapter, then compare outcomes and effects.

Adapters must define their retry and authentication limitations. In particular,
do not imply reboot-persistent idempotency when the configured runtime cache is
volatile.

The current MQTT example also keeps broker callbacks short: it reassembles a
bounded payload, places the command on a small worker queue, and executes CMPP
outside the MQTT network task. See [CMPP MQTT Binding](CMPP_MQTT_BINDING.md).
