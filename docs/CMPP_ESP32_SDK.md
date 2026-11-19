# CMPP ESP32 SDK 快速接入

`components/caelus_cmpp_sdk_esp32` 是产品无关的 ESP-IDF 集成层，当前版本为
1.0.0。它把 CMPP Core、MCP/MQTT Adapter 和 ESP32 平台能力组合成一个稳定的
产品入口，但不会定义任何最终的开关、灯控、继电器或传感器能力。

## 责任边界

SDK 负责：

- NVS 初始化、稳定 `device_uid`、显示名、mDNS hostname 和设备独有 Bearer Token；
- BLE Security 1 首次配网，及 `Caelus-*` SoftAP 恢复入口；
- 已保存 Wi-Fi 凭据的自动重连，不会在每次重启后重新进入配网；
- MCP Streamable HTTP `/mcp`、`_cmpp._tcp.local` 和运行时 Bearer 鉴权；
- 可选 MQTT 传输，并与 MCP 复用同一个 CMPP Runtime；
- CMPP 命令校验、TTL、幂等、状态版本、结果和事件。

产品工程负责：

- Device Function Module 的 capability、command、参数和安全语义；
- GPIO、PWM、I2C 等板级驱动与安全停止行为；
- 产品 `profile_id`、服务描述、容量和分区表。

## 最小接入

在产品组件的 `idf_component.yml` 中引用 SDK 所在仓库或本地组件，在 CMake 中
依赖 `caelus_cmpp_sdk_esp32`。应用启动顺序如下：

```c
#include "cmpp/cmpp_sdk_esp32.h"

void app_main(void)
{
    const cmpp_esp32_sdk_config_t config = {
        .profile_id = "vendor.product.v1",
        .server_name = "My Product",
        .server_description = "Product MCP server",
        .device_type = "my-product",
        .max_capabilities = 8,
        .idempotency_cache_size = 8,
        .max_ttl_ms = 60000,
        .persist_state_version = true,
    };
    cmpp_esp32_sdk_t *sdk = NULL;
    ESP_ERROR_CHECK(cmpp_esp32_sdk_create(&config, &sdk));

    cmpp_runtime_t *runtime = cmpp_esp32_sdk_runtime(sdk);
    /* product_board_create(runtime);  初始化硬件并注册产品 Function Module */

    ESP_ERROR_CHECK(cmpp_esp32_sdk_start(sdk));
}
```

`cmpp_esp32_sdk_create()` 必须先于硬件 Function Module 注册；
`cmpp_esp32_sdk_start()` 必须在所有 capability 注册后调用。启动后的 SDK 持有
Wi-Fi、BLE、HTTP、mDNS 和 MQTT 等长期服务，生命周期持续到设备重启。

## 配网与持久化

默认启用 BLE 配网，使用 Espressif protocomm Security 1 和 PoP。配对工具先
写入 2.4 GHz Wi-Fi，再读取并保存 `caelus.mcp.connection.v1`，最后确认配对。
因此主机无需切换到设备热点。SoftAP 门户 `http://192.168.4.1` 始终保留为
BLE 不可用时的恢复方式。

Wi-Fi、身份、Token 和“已完成配对”标记都存入 NVS。只要没有擦除 NVS，并且
`CMPP_SDK_IDENTITY_NAMESPACE` 在升级时保持不变，断电重启后设备会直接连接已保存
网络。连接失败会回到可恢复配网状态；它不代表凭据没有持久化。

配对完成且连接局域网后才发布 `_cmpp._tcp.local`。mDNS 只在同一个支持组播的
二层局域网内发现，不会跨路由器、访客网隔离或 VLAN 自动传播。

## Kconfig

| 配置项 | 含义 |
|---|---|
| `CMPP_SDK_PROVISIONING_SSID_PREFIX` | BLE 服务和 SoftAP 名称前缀 |
| `CMPP_SDK_DISPLAY_NAME_PREFIX` | 默认设备显示名前缀 |
| `CMPP_SDK_HOSTNAME_PREFIX` | 默认 `.local` hostname 前缀 |
| `CMPP_SDK_IDENTITY_NAMESPACE` | 身份与配对 NVS namespace，升级时不可随意更改 |
| `CMPP_SDK_HTTP_PORT` | MCP HTTP 端口，默认 8080 |
| `CMPP_SDK_BLE_PROVISIONING` | BLE 安全配网开关，默认启用 |
| `CMPP_SDK_BLE_POP` | 开发 PoP；量产必须每台设备唯一 |
| `CMPP_SDK_MQTT_ENABLE` | 可选 MQTT 双栈，默认关闭 |

测试板显式保留旧版 `mcp_light` namespace，已有设备升级后不会因此旋转身份或
Token。新产品应选择自己的稳定 namespace。

## 安全与容量

MCP endpoint 强制校验设备独有 Bearer Token。仓库内 vendored Espressif
`mcp-c-sdk` 2.0.1 仅增加运行时服务端鉴权回调，因为上游固定 Kconfig Token
无法承载 BLE 配网生成的每设备凭据；修改范围和来源见
[第三方依赖说明](THIRD_PARTY_NOTICES.md)。

开发默认 PoP `caelus1234` 只能用于测试。量产必须注入唯一 PoP，并通过二维码或
机身标签交付，不能把通用秘密写入所有设备。

默认 MCP+BLE 测试板镜像约 1.41 MiB，在当前 1.5 MiB app 分区中只剩约 60 KiB。
启用 MQTT、OTA、Stream 任务队列或更大的 Function Module 前，应采用 4 MB 以上
Flash 并重新规划分区。MQTT 双栈容量与 topic 约定见
[CMPP MQTT Binding](CMPP_MQTT_BINDING.md)。

## 构建验证

```bash
bash scripts/test_cmpp_host.sh
bash scripts/build.sh

idf.py -B build-mqtt \
  -DSDKCONFIG="$PWD/build-mqtt/sdkconfig" \
  -DSDKCONFIG_DEFAULTS=sdkconfig.mqtt.defaults build
```

测试板只用于演示产品层接入；复制 SDK 到新产品时不要复制
`caelus_device_function` 或 `main/board_config.*`，而应实现自己的 Function Module。
