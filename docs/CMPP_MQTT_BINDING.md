# CMPP MQTT Binding 1.0

The MQTT binding is an optional second transport for the same CMPP runtime used
by the MCP adapter. It is intended for broker-based LAN/cloud integration and
Caelus Core connectors. It does not replace the MCP Streamable HTTP endpoint.

## Topics

The default prefix is `caelus/v1`. `<uid>` is the stable CMPP device ID.

| Topic | Direction | QoS | Retained | Payload |
|---|---|---:|---:|---|
| `caelus/v1/<uid>/command` | Broker to device | 1 | No | `cmpp.command.v1` |
| `caelus/v1/<uid>/result/<command-id>` | Device to broker | 1 | No | `cmpp.command-result.v1` |
| `caelus/v1/<uid>/manifest` | Device to broker | 1 | Yes | `cmpp.manifest.v1` |
| `caelus/v1/<uid>/state` | Device to broker | 1 | Yes | `cmpp.state.v1` |
| `caelus/v1/<uid>/availability` | Device to broker | 1 | Yes | `online` or `offline` |

The device rejects retained commands so an old broker value cannot actuate
hardware after reconnect. Commands must carry a unique `command_id` and
`idempotency_key`; MQTT redelivery is handled by the normal CMPP idempotency
rules. Payloads over 4096 bytes are rejected. When four commands are already
waiting, another command is discarded with a warning; the caller should time
out and retry with the same idempotency key.

The availability topic uses MQTT Last Will. Manifest and state are republished
after every connection, and state is republished after every syntactically
valid command dispatch.

## Authentication and authorization

Broker authentication and topic ACLs are the MQTT transport boundary. Give a
device read permission only for its exact command topic and write permission
only for its own result, manifest, state, and availability topics. CMPP safety,
deadline, target, schema, and capability checks still run after broker access
is accepted.

Prefer `mqtts://` or `wss://`; firmware verifies public broker certificates
with the ESP-IDF certificate bundle. Plain `mqtt://` and `ws://` are provided
only for explicitly trusted local networks. Broker passwords are provisioned
inside the encrypted BLE session and stored in NVS; the Skill reads the
password from an environment variable so it does not appear in process
arguments. The development image stores broker credentials in NVS; production
builds must enable NVS/Flash encryption and Secure Boot. The built-in
certificate bundle validates public CAs only, so a private-CA broker needs a
future custom-CA provisioning path rather than disabling verification.

## Build profile

The original 2 MB profile keeps MQTT disabled. The dual MCP+MQTT image requires
a board with at least 4 MB Flash and uses a 3 MB factory application partition:

```bash
idf.py -B build-mqtt \
  -DSDKCONFIG="$PWD/build-mqtt/sdkconfig" \
  -DSDKCONFIG_DEFAULTS=sdkconfig.mqtt.defaults build
```

After confirming the device has at least 4 MB Flash, flash the complete image
set rather than the application binary alone:

```bash
idf.py -B build-mqtt \
  -DSDKCONFIG="$PWD/build-mqtt/sdkconfig" \
  -DSDKCONFIG_DEFAULTS=sdkconfig.mqtt.defaults \
  -p <PORT> flash monitor
```

Do not flash this profile until the fitted Flash capacity has been verified.
The profile intentionally has no OTA slots; a production OTA layout should use
larger Flash.

## Provision a broker

```bash
export CAELUS_MQTT_PASSWORD='broker-secret'
python3 skills/caelus-pair-device/scripts/caelus_ble.py provision --yes \
  --mqtt-uri mqtts://broker.example:8883 \
  --mqtt-username caelus-device
```

Omit the MQTT options to keep the device MCP-only. The generic binding makes
the device usable from any MQTT client or bridge, but does not itself publish
Home Assistant, Alexa, or vendor-specific entity discovery documents. Those
product mappings belong in a thin Caelus Core connector or profile adapter,
not in CMPP Core.
