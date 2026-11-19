# CMPP SDK 1.0

This directory contains the first transport-neutral CMPP SDK release. The SDK
components remain product-independent. The test-board application demonstrates
their integration through a separate Device Function Module and board assembly
layer.

For flashing, first-time provisioning, persistence behavior and hardware
bring-up checks, see the [test-board deployment guide](TEST_BOARD_DEPLOYMENT.md).

## Components

| Component | Responsibility | Public dependencies |
|---|---|---|
| `caelus_cmpp_core` | Registration, validation, deadlines, dispatch, idempotency, receipts, events and state versions | ISO C only |
| `caelus_cmpp_platform_esp32` | FreeRTOS locks, monotonic clock, NVS, random, logging and configurable allocation | Core + ESP-IDF |
| `caelus_cmpp_codec_json` | CMPP value/request/result/manifest JSON conversion | Core + cJSON |
| `caelus_cmpp_adapter_mcp` | Official MCP tool/resource/notification mapping | Core + codec + `mcp-c-sdk` |
| `caelus_cmpp_adapter_mqtt` | Optional MQTT topics and JSON command/result mapping | Core + codec |
| `caelus_cmpp_sdk_esp32` | Stable identity, BLE/SoftAP provisioning, authenticated MCP/mDNS service and optional MQTT lifecycle | Core + adapters + ESP-IDF |
| `caelus_cmpp_backend_pwm` | Generic normalized LEDC output and ramping | Core + LEDC |
| `caelus_cmpp_backend_relay` | Generic binary GPIO output and switch interval enforcement | Core + GPIO |
| `caelus_device_function` | Test-board capability semantics for six indicators and four relays | Core + generic backend APIs |

The Core has no ESP-IDF, cJSON, MCP, GPIO or product headers. Function Modules
use only [`cmpp.h`](../components/caelus_cmpp_core/include/cmpp/cmpp.h) to
register descriptors and callbacks.

## Runtime lifecycle

For normal ESP32 products, `caelus_cmpp_sdk_esp32` owns NVS, platform,
identity, provisioning and transport startup:

1. Call `cmpp_esp32_sdk_create()` with product metadata and capacity limits.
2. Obtain the shared runtime with `cmpp_esp32_sdk_runtime()`.
3. Initialize hardware backends in the product assembly layer.
4. Register every Function Module through
   `cmpp_runtime_register_capability()`.
5. Call `cmpp_esp32_sdk_start()` to provision or connect Wi-Fi, publish mDNS,
   and start authenticated MCP plus optional MQTT.

The SDK is designed to live until reboot after it starts. Products needing a
custom lifecycle can continue assembling the lower-level components directly.
See [ESP32 SDK quick start](CMPP_ESP32_SDK.md) for code and configuration.

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
mapping, equivalent MCP/MQTT/in-process semantics, and the test-board Function
Module's command, dry-run, observation and safe-stop contracts.

The MQTT wire contract, security boundary, capacity limits, and 4 MB build
profile are documented in [CMPP MQTT Binding](CMPP_MQTT_BINDING.md).

Compile the ESP32 SDK and test-board example with:

```bash
bash scripts/build.sh
```
