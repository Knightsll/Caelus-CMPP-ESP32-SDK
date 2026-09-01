# CMPP ESP32 SDK 0.1

This directory contains the first transport-neutral CMPP SDK release. The SDK
components remain product-independent. The test-board application demonstrates
their integration through a separate Device Function Module and board assembly
layer.

## Components

| Component | Responsibility | Public dependencies |
|---|---|---|
| `caelus_cmpp_core` | Registration, validation, deadlines, dispatch, idempotency, receipts, events and state versions | ISO C only |
| `caelus_cmpp_platform_esp32` | FreeRTOS locks, monotonic clock, NVS, random, logging and configurable allocation | Core + ESP-IDF |
| `caelus_cmpp_codec_json` | CMPP value/request/result/manifest JSON conversion | Core + cJSON |
| `caelus_cmpp_adapter_mcp` | Official MCP tool/resource/notification mapping | Core + codec + `mcp-c-sdk` |
| `caelus_cmpp_backend_pwm` | Generic normalized LEDC output and ramping | Core + LEDC |
| `caelus_cmpp_backend_relay` | Generic binary GPIO output and switch interval enforcement | Core + GPIO |
| `caelus_device_function` | Test-board capability semantics for six indicators and four relays | Core + generic backend APIs |

The Core has no ESP-IDF, cJSON, MCP, GPIO or product headers. Function Modules
use only [`cmpp.h`](../components/caelus_cmpp_core/include/cmpp/cmpp.h) to
register descriptors and callbacks.

## Runtime lifecycle

1. Initialize NVS in the application.
2. Create `cmpp_esp32_platform_t` and obtain its operations table.
3. Create `cmpp_runtime_t` with a stable `device_id` and `profile_id`.
4. Initialize hardware backends in the board assembly layer.
5. Register every Function Module through
   `cmpp_runtime_register_capability()`.
6. Dispatch in-process commands directly or attach an adapter.
7. On shutdown, call `cmpp_runtime_safe_stop()`, destroy adapters and Function
   Modules, destroy hardware backends, destroy the runtime, then destroy the
   platform object.

Descriptors and callback tables are borrowed for the lifetime of the runtime.
Request values are borrowed for one dispatch. Response values are owned by the
runtime allocator and must be released with `cmpp_response_deinit()`.
Observed values returned by `cmpp_runtime_observe()` are owned by the runtime
allocator and must be released with `cmpp_value_deinit()`.

## Execution guarantees

Every accepted command follows the documented order: envelope and target
validation, capability/argument/safety validation, initial deadline and
idempotency checks, execution-guard acquisition, deadline and idempotency
rechecks, admission, execution, result validation, state-version update, event
publication, then result caching.

Execution is serialized per runtime. Exact retries return a cloned cached
receipt without executing again. Reuse of either a command ID or idempotency key
with different content is rejected. A committed effect increments the 64-bit
state version even when the command outcome is `failed`; this preserves the
fact that hardware may already have changed.

Event callbacks run synchronously while the execution guard is held. They must
not redispatch into the same runtime and should do bounded work.

## Configuration and memory

Long-lived Core memory is proportional to `max_capabilities` and
`idempotency_cache_size`. Each cached result owns a deep copy of its optional
CMPP value. Set the cache size from the product's retry window and payload
budget. The default ESP32 platform allocator uses `calloc/free`; products can
provide a bounded or capability-aware allocator.

`persist_state_version` stores the committed version under NVS key
`cmpp.state_ver`. Command-result cache entries intentionally do not survive a
reboot in this release; this limitation is published through MCP.

## Tests

Run the transport-independent contract suite with:

```bash
bash scripts/test_cmpp_host.sh
```

It covers validation, target rejection, deadline expiry while waiting,
concurrent serialization, exact retries, ID/key collisions, failure effects,
monotonic and persistent state versions, event/receipt consistency, JSON
mapping, equivalent MCP/in-process semantics, and the test-board Function
Module's command, dry-run, observation and safe-stop contracts.

Compile all ESP32 components with:

```bash
bash scripts/build.sh
```
