# ESP32 CMPP SDK — Codex Implementation Brief

## Status

- State: Implemented for the first test board; physical board validation pending
- Implementation authorized: Yes — SDK scope first, then the supplied board schematic
- Target platform: ESP32 / ESP-IDF
- Scope: CMPP SDK, MCP Adapter, PWM/Relay Backend, and a separate test-board Device Function Module
- Excluded: Caelus Kernel

Implementation began after the SDK brief was approved. The test-board Function
Module and application assembly were added only after the board schematic and
GPIO definition were supplied.

---

## 1. Objective

Implement a production-ready CMPP SDK for ESP32 in one complete delivery.

CMPP is the authoritative contract. MCP is only one transport adapter. The reusable SDK must remain independent from:

- MCP and its protocol version
- Any concrete device function module
- GPIO, PWM, relay, LED, or board-specific implementation
- Caelus Kernel

The first release must provide reusable PWM and relay backends. Other hardware backends are extension points and are marked as **to be continued**; they must not be delivered as incomplete production stubs.

### 1.1 Terminology

`Light` and the current `MCP-Light` project name are legacy placeholders for the actual function carried by the ESP32. They MUST NOT be interpreted as a fixed lighting capability, data model, or SDK component.

In this document:

- **Device Function Module** means the actual product function hosted by the ESP32.
- **Function Module** is the generic extension point through which that product function registers CMPP capabilities.
- For this test board, the supplied hardware definition selects six PWM
  indicators and four relay outputs; these remain outside the reusable SDK.

### 1.2 CMPP and MCP relationship

CMPP is an independent device-capability contract and runtime model. MCP is one official binding used to expose CMPP over MCP tools, resources, results, and notifications.

The architecture has three distinct layers:

1. **CMPP Domain Contract**: capabilities, commands, state, receipts, events, safety, deadlines, and idempotency.
2. **CMPP Core Runtime**: registration, validation, dispatch, execution lifecycle, and state management.
3. **Bindings and Adapters**: MCP, or future HTTP, BLE, UART, IPC, and in-process adapters.

Mandatory rules:

- CMPP Core MUST build, link, initialize, and execute commands without `mcp-c-sdk` or any MCP component.
- The MCP Adapter MUST implement the documented **CMPP MCP Binding**.
- MCP version negotiation and MCP-specific objects MUST remain inside the MCP Adapter.
- Replacing or disabling the MCP Adapter MUST NOT require changes to CMPP Core or any Function Module.
- A non-MCP adapter may carry the same CMPP semantics, but it MUST NOT claim compatibility with the CMPP MCP Binding unless it implements that binding.
- Function Modules MUST receive transport-neutral CMPP requests and MUST NOT be able to determine whether a request originated from MCP, HTTP, BLE, UART, or an in-process caller.

---

## 2. Mandatory dependency direction

Allowed dependency graph:

```text
MCP Adapter ───────────────┐
Other Adapter ─────────────┼────────> CMPP Core ──────> Platform Abstraction
Device Function Module ────┘                                │
          │                                                  ▼
          └── optional ──> PWM/Relay Backend ────────> ESP32 Platform Port
```

The diagram shows one representative Device Function Module node. It uses CMPP Core to register and execute capabilities, and it may optionally use one or more hardware backends such as PWM or relay. A product may register one or multiple independent Function Modules; each module follows the same dependency rules.

Forbidden dependencies:

```text
CMPP Core    -X-> MCP Adapter
CMPP Core    -X-> Any transport adapter
CMPP Core    -X-> Device Function Module
CMPP Core    -X-> ESP-IDF headers
MCP Adapter  -X-> Function Module internal APIs
PWM/Relay    -X-> MCP
```

Public `cmpp_core` headers MUST NOT expose MCP, product-function, GPIO, LEDC, FreeRTOS, or ESP-IDF types.

---

## 3. Components

### 3.1 `cmpp_core`

Responsibilities:

- Device and profile descriptors
- Dynamic capability registration
- Command envelope validation
- Target and capability validation
- Safety metadata validation
- TTL and deadline enforcement
- Idempotency and command-ID collision detection
- Execution serialization and race-safe deadline recheck
- Command receipt generation
- Result and event separation
- Device state versioning
- Health and limitations reporting
- Storage, clock, lock, allocator, random, and logging abstractions

Not responsible for:

- JSON-RPC or MCP sessions
- HTTP, mDNS, or transport authentication
- MCP tools, resources, notifications, or protocol-version negotiation
- GPIO, PWM, or relay operations
- Product-specific function semantics
- Pairing or network provisioning

### 3.2 `cmpp_platform_esp32`

Implement the CMPP platform abstraction using mature ESP-IDF services:

- FreeRTOS synchronization
- `esp_timer`
- NVS
- ESP logging
- ESP random
- Configurable allocator

### 3.3 `cmpp_codec_json`

Convert between CMPP-neutral values and JSON.

Use `cJSON` internally, but do not expose `cJSON` types through the CMPP Core public API.

### 3.4 `cmpp_adapter_mcp`

Implement the official CMPP MCP Binding by mapping MCP operations to CMPP operations:

- MCP tools to CMPP commands
- CMPP receipts/results to MCP structured results
- CMPP state and manifest to MCP resources
- CMPP events to supported MCP notification mechanisms
- Authentication and discovery integration

The adapter must not contain CMPP execution logic.

MCP protocol version is not a product requirement. Reuse the currently stable `espressif/mcp-c-sdk` unless compatibility testing shows a blocking defect.

The component dependency on `mcp-c-sdk`, HTTP transport, MCP sessions, and MCP protocol types must terminate at this adapter boundary. `cmpp_core` and Function Modules must not transitively depend on them.

### 3.5 `cmpp_backend_pwm`

Provide a generic PWM actuator backend:

- Configurable channel and output pin
- Configurable frequency and resolution
- Normalized output range
- Optional inversion
- Minimum/maximum output constraints
- Safe startup and safe-stop output
- Optional transition/ramp support
- Observable current output state

It must not contain product-specific names or behavior.

### 3.6 `cmpp_backend_relay`

Provide a generic binary actuator backend:

- Configurable GPIO
- Active-high or active-low
- Safe boot state
- Safe-stop state
- Optional minimum switch interval
- Observable current state

It must not contain product-specific names or behavior.

### 3.7 Device Function Module

Implement the actual ESP32 product function as a replaceable module after the final function definition is provided.

The module owns:

- Capability names and descriptors
- Command names and argument schemas
- Product state model
- Capability-specific validation and admission
- Safety rules and limitation reporting
- Mapping between product semantics and PWM/relay operations
- Optional observation, cancellation, and safe-stop behavior

The module must register itself only through the public CMPP Core API. The MCP Adapter must discover its capabilities through CMPP descriptors and must not include product-specific dispatch branches.

The final board pin configuration belongs to the application assembly layer, not to the Function Module or CMPP Core.

---

## 4. Core public API direction

The exact API may be refined during implementation, but it must preserve these boundaries:

```c
cmpp_status_t cmpp_runtime_create(
    const cmpp_platform_ops_t *platform,
    const cmpp_runtime_config_t *config,
    cmpp_runtime_t **runtime
);

cmpp_status_t cmpp_runtime_register_capability(
    cmpp_runtime_t *runtime,
    const cmpp_capability_descriptor_t *descriptor,
    const cmpp_capability_ops_t *ops,
    void *module_context
);

cmpp_status_t cmpp_runtime_dispatch(
    cmpp_runtime_t *runtime,
    const cmpp_request_t *request,
    cmpp_response_t *response
);
```

Functional module operations:

```c
typedef struct {
    cmpp_status_t (*validate)(
        void *context,
        const cmpp_value_t *arguments
    );

    cmpp_status_t (*admit)(
        void *context,
        const cmpp_command_t *command
    );

    cmpp_status_t (*execute)(
        void *context,
        const cmpp_command_t *command,
        cmpp_result_t *result
    );

    cmpp_status_t (*observe)(
        void *context,
        cmpp_value_t *state
    );

    cmpp_status_t (*cancel)(
        void *context,
        const cmpp_command_id_t *command_id
    );

    cmpp_status_t (*safe_stop)(
        void *context
    );
} cmpp_capability_ops_t;
```

Capabilities must be registered through descriptors and callbacks. Do not use a fixed enum that prevents third-party capability modules.

---

## 5. Command execution pipeline

Every command must follow this order:

1. Validate the CMPP envelope.
2. Validate the device target.
3. Resolve the registered capability.
4. Validate arguments and safety metadata.
5. Check TTL/deadline.
6. Check idempotency and command-ID collision.
7. Acquire the execution guard.
8. Recheck TTL/deadline after acquiring the guard.
9. Run capability admission.
10. Execute the capability callback.
11. Validate the returned result state.
12. Update state version when committed.
13. Generate the CMPP receipt.
14. Publish the corresponding event.
15. Store the idempotent result.

Invalid state combinations must be rejected, including:

- `rejected + committed`
- `completed + unknown`
- Duplicate command execution after a cached committed result
- Reusing one command ID with different command content

---

## 6. Extensibility requirements

A third-party module must be able to add a capability without modifying:

- `cmpp_core`
- MCP Adapter
- Existing Device Function Module
- PWM or relay backend

A future transport adapter must be able to invoke the same CMPP runtime without depending on MCP.

The SDK must include a transport-neutral in-process dispatch path that can be used by tests and by future adapters. Adding an adapter must not add protocol-specific branches to `cmpp_core`.

Potential future backends such as addressable LED, RGB/RGBW, motor, and sensor are explicitly outside this delivery.

---

## 7. Third-party library policy

Prefer mature existing components:

- `espressif/mcp-c-sdk`
- `espressif/cjson`
- ESP-IDF NVS
- FreeRTOS
- `esp_timer`
- ESP HTTP server
- ESP mDNS
- mbedTLS
- ESP-IDF LEDC for PWM

Rules:

- Pin dependency versions.
- Record dependency licenses and provenance.
- Do not directly edit generated `managed_components`.
- Use a versioned component override when customization is unavoidable.
- Do not introduce a heavyweight general JSON Schema engine unless resource measurements justify it.

---

## 8. Proposed source layout

```text
components/
  caelus_cmpp_core/
    include/
    src/
    test/

  caelus_cmpp_platform_esp32/
    include/
    src/

  caelus_cmpp_codec_json/
    include/
    src/
    test/

  caelus_cmpp_adapter_mcp/
    include/
    src/
    test/

  caelus_cmpp_backend_pwm/
    include/
    src/
    test/

  caelus_cmpp_backend_relay/
    include/
    src/
    test/

  caelus_device_function/
    include/
    src/
    test/

main/
  app_main.c
  board_config.c
  board_config.h
```

Names may be shortened if needed, but component boundaries must remain.

---

## 9. One-shot delivery requirements

The final change set must include:

- Complete implementation
- No production TODOs or placeholder branches
- No incomplete future hardware backends
- Unit and contract tests
- On-target integration tests
- Updated build configuration
- Dependency lock state
- Public API documentation
- Capability module development guide
- Transport Adapter development guide
- CMPP MCP Binding specification
- MCP Adapter mapping documentation
- PWM example
- Relay example
- Test-board Device Function Module and application assembly
- Migration notes from the current implementation

Internal compile-and-test iterations are allowed. **One-shot** means the submitted result must be complete and reviewable as a single delivery.

---

## 10. Acceptance tests

Required coverage:

- Valid CMPP command
- Invalid envelope and arguments
- Wrong target or unsupported capability
- Expired command
- Command expiring while waiting for execution lock
- Exact idempotent retry
- Command-ID collision
- Concurrent requests
- Execution failure before and after hardware commit
- State-version monotonicity
- Receipt/event consistency
- Reboot and persistent identity/state behavior
- Authentication rejection and acceptance
- MCP-to-CMPP mapping
- `cmpp_core` build and execution with the MCP Adapter and `mcp-c-sdk` excluded
- In-process transport-neutral dispatch
- Equivalent CMPP command semantics through MCP and a non-MCP test adapter
- MCP-version-specific data contained entirely inside the MCP Adapter
- PWM normal, inverted, and safe-stop behavior
- Relay active-high, active-low, and safe boot behavior
- Generic PWM backend behavior and state observation
- Generic relay backend behavior and state observation
- Test-board Device Function Module over its selected backends
- Unsupported command/actuator combinations reported through CMPP limitations
- Long-running heap stability

Resource gates:

- Preserve at least 20% application partition headroom.
- Target no more than 32 KiB flash growth over the current firmware.
- Target no more than 12 KiB long-lived static DRAM for the reusable SDK.
- No persistent heap decline during repeated command execution.
- Only one production command execution path.

If a resource target cannot be met, Codex must report measurements and request approval before relaxing it.

---

## 11. Required hardware input

Implementation must not begin until the following are confirmed:

- Exact ESP32 chip/module
- Actual function carried by the ESP32
- Capability names, commands, arguments, and observable state
- Required safety rules and interlocks
- PWM output pins
- Relay output pins
- Active-high/active-low configuration
- PWM frequency requirements
- Boot output states
- Safe-stop states
- Minimum/maximum output limits
- Whether PWM and relay coexist on one device
- Power-enable or auxiliary control pins
- Any input, feedback, sensor, or fault-detection pins

---

## 12. Implementation gate

Codex must first:

1. Inspect the approved brief.
2. Inspect the final hardware definition.
3. Report any contradiction or impossible resource requirement.
4. Wait for explicit implementation authorization.

Only after authorization may Codex modify source code.
