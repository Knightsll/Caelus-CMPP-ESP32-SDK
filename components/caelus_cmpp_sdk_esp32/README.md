# Caelus CMPP ESP32 SDK

Reusable ESP-IDF integration for a CMPP device. It owns stable device identity,
secure BLE provisioning with SoftAP recovery, Wi-Fi persistence, authenticated
MCP Streamable HTTP, `_cmpp._tcp.local` discovery and optional MQTT transport.

It intentionally does not contain a final Device Function Module or board
driver. A product creates the SDK, registers its own capabilities on the SDK's
runtime, and then starts the SDK:

```c
#include "cmpp/cmpp_sdk_esp32.h"

cmpp_esp32_sdk_t *sdk = NULL;
const cmpp_esp32_sdk_config_t config = {
    .profile_id = "vendor.product.v1",
    .server_name = "Product Name",
    .server_description = "Product description",
    .device_type = "product-type",
    .max_capabilities = 8,
    .idempotency_cache_size = 8,
    .max_ttl_ms = 60000,
    .persist_state_version = true,
};
ESP_ERROR_CHECK(cmpp_esp32_sdk_create(&config, &sdk));

cmpp_runtime_t *runtime = cmpp_esp32_sdk_runtime(sdk);
/* Initialize the board and register product capabilities on runtime here. */

ESP_ERROR_CHECK(cmpp_esp32_sdk_start(sdk));
```

See [`docs/CMPP_ESP32_SDK.md`](../../docs/CMPP_ESP32_SDK.md) for configuration,
provisioning, lifecycle and production security requirements.
