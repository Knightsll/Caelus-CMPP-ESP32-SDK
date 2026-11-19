# Caelus unattended pairing protocol

## State flow

```text
device advertises Caelus-* BLE plus recovery SoftAP
→ Agent obtains one-time authorization for local secret use
→ Agent reads saved 2.4 GHz profile from NetworkManager
→ Agent persists or resumes a local caelus.pairing.task.v1 record
→ Agent establishes an Espressif Security 1 BLE session
→ Agent uses prov-config and secure custom-data
→ device validates and persists household Wi-Fi
→ device returns a compact UID/Token response
→ Agent validates and atomically stores the credential
→ Agent reloads and byte-compares the validated credential
→ Agent sends secure custom-data complete
→ device stops BLE and recovery SoftAP
→ current device joins LAN and publishes _cmpp._tcp.local
→ Agent performs authenticated read-only MCP verification
→ adapter remains disabled until separately enabled and approved
```

No host Wi-Fi switch, browser, serial console, password prompt, or completion
click is part of the normal BLE run. The web UI at `http://192.168.4.1` and the
NetworkManager transaction below remain the SoftAP recovery route.

## Setup portal API

The setup AP gateway is always `192.168.4.1`. Disable HTTP proxy inheritance for portal calls.

### Resume pending pairing

`GET /api/pairing`

- `200` with `ok`, `device`, and `connection` resumes a previously configured pairing. Reuse the returned identity and Token; do not configure Wi-Fi again.
- `404` means no resumable pairing exists; continue with discovery/configuration.

### Read suggestions and scan results

`GET /api/networks`

Expected fields include `ok`, `networks`, `suggested_name`, and `suggested_hostname`. The Agent may accept suggestions or apply user-provided `--display-name` and `--hostname`. A hostname excludes `.local` and must match `[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?`.

### Configure household network

`POST /api/configure` with JSON:

```json
{
  "display_name": "Caelus Board 1",
  "hostname": "caelus-board-1",
  "ssid": "household SSID",
  "password": "saved NetworkManager PSK"
}
```

Send the PSK only in the local HTTP request body. Never place it in command arguments or output. Current firmware supports open or PSK-based 2.4 GHz networks; reject enterprise/EAP profiles in the automated client.

On success, the response includes `device` and `connection`. Validate, durably store, reload, and compare it before completing pairing.

### Complete pairing

`POST /api/complete` with `{}` only after credential persistence succeeds. This marks the device paired and permits the setup AP to close. If the host fails before this call, reconnect and use `/api/pairing` to recover the same connection data.

## NetworkManager transaction

1. Resolve the active Wi-Fi profile or an explicit `--wifi-connection`.
2. Read `802-11-wireless.ssid`, security mode, and—only when required—the saved PSK with `nmcli --show-secrets`. An explicit `--wifi-ssid` may replace only the target SSID while retaining that profile as the PSK source. Require user confirmation that both SSIDs share credentials; never infer this from naming.
3. Scan the selected interface for `Caelus-*`. Auto-select only one exact result.
4. Persist a Pairing Task before creating or activating a temporary profile. Never put the PSK or Bearer Token in the task.
5. Add a uniquely named temporary profile with `connection.autoconnect no`.
6. Mark restoration necessary before attempting to activate that profile, because activation may partially change network state even when it returns an error.
7. In `finally`, activate the original profile and delete both the current and any task-correlated stale temporary profiles.
8. If the active profile after a prior crash is named `caelus-provision-*`, recover the household profile from exactly one matching resumable task. Never reuse the setup profile as the household credential source.
9. Blank in-process PSK references as soon as portal work ends. Python cannot guarantee physical memory erasure, so avoid unnecessary copies and never serialize the secret.

The `--yes` flag records the user's authorization for temporary Wi-Fi switching and local transmission of the saved PSK. It is not permission to execute MCP actuator tools.

## Connection schema

Current firmware returns this shape:

```json
{
  "schema": "caelus.mcp.connection.v1",
  "name": "Caelus Board 1",
  "device_uid": "hw_0123456789abcdef",
  "hostname": "caelus-board-1",
  "transport": "streamable_http",
  "url": "http://caelus-board-1.local:8080/mcp",
  "headers": {
    "Authorization": "Bearer 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  },
  "discovery": "_cmpp._tcp.local"
}
```

Required validation:

- Require schema `caelus.mcp.connection.v1`.
- Use `device_uid`, not an IP address, as stable identity.
- Accept only Streamable HTTP and `/mcp` on `.local` or a private/link-local IP.
- Require a 256-bit Bearer Token represented by 64 hexadecimal characters.
- Accept native `_cmpp._tcp.local` and legacy `_caelus-hw._tcp.local`; use the value to select the verification contract.
- Treat a structurally valid connection as unverified until authenticated manifest verification succeeds.

## Credential registry

The bundled script stores:

```json
{
  "registry_schema": "caelus.mcp.device-registry.v1",
  "pairing_tasks": {
    "pair_0123456789abcdef": {
      "schema": "caelus.pairing.task.v1",
      "state": "completed",
      "credential_state": "stored",
      "verification_state": "verified"
    }
  },
  "devices": {
    "hw_0123456789abcdef": {
      "connection": {"schema": "caelus.mcp.connection.v1"},
      "contract": "native_cmpp_v1",
      "saved_at": "ISO-8601 timestamp",
      "pairing": {"state": "completed"},
      "credential": {"state": "stored"},
      "verification": {
        "state": "verified",
        "verified_at": "ISO-8601 timestamp",
        "manifest": {"schema": "cmpp.manifest.v1"},
        "manifest_uri": "cmpp://manifest",
        "vendor_attestation": "unavailable"
      },
      "plugin": {"state": "disabled"},
      "grant": {"state": "not_granted"}
    }
  }
}
```

The store is secret: parent directory mode `0700`, file mode `0600`, and atomic replacement. Listing and ordinary display mask Authorization. Pairing tasks make network recovery observable but contain no PSK or Bearer Token. Credential, verification, plugin enablement, and grant remain independent axes.

## Authenticated profile verification

Use MCP protocol `2025-11-25`:

1. `initialize`; retain `MCP-Session-Id`.
2. Send `notifications/initialized`.
3. Call `tools/list` without invoking a tool.
4. Call `resources/list` and require the manifest URI selected by the saved discovery contract.
5. Read that single manifest resource and validate it; do not call any tool.

For native `_cmpp._tcp.local` firmware:

- Require tool `cmpp.execute` and resource `cmpp://manifest`, but never invoke the tool.
- Require manifest schema `cmpp.manifest.v1`, `protocol_version: 1.0`, and `device_id` matching the saved `device_uid`.
- Require a non-empty `profile_id`, non-negative integer `state_version`, unique capability IDs, non-empty command types, and valid safety/ack/dry-run descriptor fields.
- For profile `caelus.test-board.v1`, require both `test_board.pwm` and `test_board.relay`.
- Record `device_profile_verified: true` only after these checks. Current native firmware exposes no signed vendor or publisher identity in the manifest, so record `vendor_attestation: unavailable` and `official_device: false` rather than making an unsupported attestation claim.

For legacy `_caelus-hw._tcp.local` firmware:

- Require resource `caelus://hardware/manifest`, schema `caelus.hardware.manifest.v0`, HAP `0.1.0`, `vendor == Caelus`, matching `device_uid`, paired state, and the declared transport/capability/command/privacy fields.
- Require a transport matching the saved connection and `x_cmpp_compatibility` declaring MCP `2025-11-25`, connection schema `caelus.mcp.connection.v1`, native target `caelus.hardware.profile.v0`, and `connector_required: true`.

Tool presence is discovery, not permission to execute. Pairing verification must never invoke `cmpp.execute`, `caelus.hardware.execute_approved_command`, or another actuator tool.

## Discovery and trust boundary

Browse native `_cmpp._tcp` and legacy `_caelus-hw._tcp` with Avahi. Native firmware currently advertises `path`, `transport`, `device_type`, `cmpp_version`, `mcp_version`, `device_uid`, `display_name`, `pairing`, `auth`, and `firmware`. mDNS is unauthenticated; use it to locate a candidate, then authenticate and compare the manifest identity. Never replace a trusted credential solely from TXT data.

Current prototype transport is local HTTP with Bearer authentication and declares `encrypted: false`; a compromised LAN can observe the Token. Production official hardware should use an authenticated setup secret plus HTTPS/mTLS or equivalent encryption.

Pairing is not adapter enablement. Installing or running this Skill does not enable capabilities. An Agent must not convert intent directly into `cmpp.command.v1` or invoke `cmpp.execute`; the Action Broker and hard policy gate own command construction and approval.
