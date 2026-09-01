# CMPP Capability Module Guide

A Function Module owns product semantics. It declares capability and command
names, validates arguments, applies admission and safety rules, invokes a
backend, and reports the real effect. It must not include MCP or transport
headers.

## Registration

Keep descriptors and command-name arrays in static storage because the runtime
borrows them:

```c
static const char *const command_types[] = {"fan.speed.set"};

static const cmpp_capability_descriptor_t fan_descriptor = {
    .capability_id = "fan.speed",
    .display_name = "Fan speed",
    .version = "1.0.0",
    .description = "Bounded normalized fan output",
    .arguments_schema_json =
        "{\"type\":\"object\",\"required\":[\"output\"],"
        "\"properties\":{\"output\":{\"type\":\"integer\","
        "\"minimum\":0,\"maximum\":10000}}}",
    .command_types = command_types,
    .command_type_count = 1,
    .allowed_safety_mask = CMPP_SAFETY_MASK(CMPP_SAFETY_REVERSIBLE),
    .requires_ack = true,
};
```

`validate` checks argument type/range without side effects. `admit` evaluates
current product interlocks immediately before execution. `execute` performs the
operation and must distinguish these cases:

| Hardware observation | Outcome | Effect |
|---|---|---|
| Rejected before touching hardware | `rejected` | `no_effect` |
| Completed successfully | `completed` | `committed` |
| Driver failed before commit is possible | `failed` | `no_effect` |
| Driver failed after a known commit | `failed` | `committed` |
| Driver failed and commit cannot be determined | `failed` | `unknown` |

`completed + unknown` and `rejected + committed` are invalid and Core converts
them to a safe internal failure. Returned data is borrowed during the callback;
Core deep-copies it into the receipt.

`observe` returns the module's current state as a borrowed CMPP value. State
storage must remain valid for the whole callback; immediately after the
callback completes, Core deep-copies it for the caller. `safe_stop` must be
idempotent and must
prefer the declared safe condition over normal rate limits. `cancel` is only
needed when the module implements cancellable work.

Register the module only after its backend is initialized:

```c
cmpp_capability_ops_t operations = {
    .validate = fan_validate,
    .admit = fan_admit,
    .execute = fan_execute,
    .observe = fan_observe,
    .safe_stop = fan_safe_stop,
};

cmpp_status_t status = cmpp_runtime_register_capability(
    runtime, &fan_descriptor, &operations, &fan_context);
```

Board pins, polarity, startup levels and electrical limits belong in the
application assembly layer. Pass a configured generic backend handle into the
module context instead of embedding board constants in the module.
