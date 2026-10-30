# Caelus BLE provisioning contract v1

## Discovery

- BLE name: `Caelus-*`, normally the same as the recovery SoftAP SSID.
- Primary service UUID: `021a9004-0382-4aea-bff4-6b3f1c5adfb4`.
- Protocol: Espressif `network_provisioning` 1.4 session-only mode.
- Capability marker: `caelus_pairing_v1` in `proto-ver`.
- Standard endpoints: `prov-session`, `proto-ver`, and, when Wi-Fi is not yet
  saved, `prov-config`, `prov-scan`, and `prov-ctrl`.
- Application endpoint: `custom-data`.

Discovery is local radio scope. Unlike mDNS, it does not require the host and
device to already share a LAN.

## Security

Version 1 uses Espressif protocomm Security 1: X25519 key exchange, proof of
possession, and AES-CTR. The first test-board firmware uses the development PoP
`caelus1234`. This is suitable only for bring-up. Production boards must have a
unique random per-device PoP provisioned at manufacturing time and printed as a
QR code or enclosure label. The Skill reads an override from `CAELUS_BLE_POP`.

Do not silently ship the development PoP as a production credential. Security
2 (SRP6a + AES-GCM) is the preferred production upgrade once the manufacturing
pipeline can inject unique salt/verifier data.

## Custom endpoint messages

Messages are UTF-8 JSON inside the established protocomm security session.

Pair request:

```json
{"action":"pair","display_name":"Desk lamp","hostname":"desk-lamp"}
```

Pair response is deliberately compact to stay below the BLE characteristic
payload limit:

```json
{"ok":true,"device_uid":"hw_...","auth_token":"64 hex characters"}
```

The client constructs and validates the standard
`caelus.mcp.connection.v1` record, saves it atomically, reloads it, and only
then sends:

```json
{"action":"complete"}
```

Success response:

```json
{"ok":true,"message":"pairing_complete"}
```

After that response grace period, firmware deinitializes the provisioning
manager, stops BLE and the recovery SoftAP, and releases BLE runtime memory.

## Recovery

If Wi-Fi was saved but `complete` was not received, the next boot advertises
BLE and SoftAP again. The BLE `proto-ver` document omits `wifi_prov`; the client
skips Wi-Fi configuration, requests the credential again, persists it, and
retries `complete`.

SoftAP remains the compatibility path through `/api/networks`,
`/api/configure`, `/api/pairing`, and `/api/complete` at `192.168.4.1`.
