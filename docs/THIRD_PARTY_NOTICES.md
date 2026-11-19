# CMPP SDK Dependency Provenance

| Dependency | Pinned version | Source | License | Usage boundary |
|---|---:|---|---|---|
| ESP-IDF | 6.0.2 | Espressif ESP-IDF distribution | Apache-2.0 and component notices | ESP32 platform and hardware ports |
| espressif/mcp-c-sdk | 2.0.1 | Vendored from Espressif Component Registry / `esp-iot-solution` commit `001a3a10f668691b175887661c0cc3bf88e7b8d0` | Apache-2.0 | MCP adapter and HTTP transport only |
| espressif/cjson | 1.7.19~2 | Espressif Component Registry | MIT | JSON codec and MCP adapter only |
| espressif/mqtt | 1.1.0 | Espressif Component Registry | Apache-2.0 | Optional ESP32 MQTT client transport only |

Exact resolved hashes and transitive components are recorded in
[`dependencies.lock`](../dependencies.lock). The vendored MCP component retains
its upstream license in `components/mcp-c-sdk/license.txt`. It has a small,
source-visible extension that lets the HTTP server validate a runtime
per-device Bearer credential; this is required because provisioning creates a
unique credential and the upstream 2.0.1 server otherwise accepts only a
compile-time fixed token. Other managed component sources are not modified.
