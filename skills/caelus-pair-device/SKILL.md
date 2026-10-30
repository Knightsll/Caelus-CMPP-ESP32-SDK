---
name: caelus-pair-device
description: Discover, provision, recover, register, verify, list, or remove Caelus LAN hardware MCP devices. Prefer secure Bluetooth LE provisioning without switching the host Wi-Fi, and retain the Caelus-* SoftAP portal as a fallback. Use for first boot, ESP32 setup, Caelus BLE discovery, `192.168.4.1`, `_cmpp._tcp.local`, `.mcp.json`, lost discovery, MCP Bearer credentials, or interrupted pairing.
---

# Pair a Caelus device

Prefer Bluetooth LE for first-time setup. While pairing is incomplete, current
firmware advertises a `Caelus-*` BLE device using Espressif protocomm service
UUID `021a9004-0382-4aea-bff4-6b3f1c5adfb4`. The same device also exposes its
`Caelus-*` SoftAP portal as a recovery path. Completed devices stop both setup
advertisements and publish `_cmpp._tcp.local` only after joining the LAN.

Treat discovery, Wi-Fi provisioning, MCP credential persistence, device
pairing completion, device-profile verification, vendor attestation, plugin
enablement, and capability grants as separate states. Pairing registers the
transport and identity; it never grants actuator permission.

Read [references/installation.md](references/installation.md) only when
installing this Skill in Codex or Hermes. Read
[references/pairing-protocol.md](references/pairing-protocol.md) when
diagnosing protocol, credential, MCP, or recovery failures. Read
[references/ble-provisioning.md](references/ble-provisioning.md) when changing
firmware, BLE security, service UUIDs, or production provisioning policy. Read
[references/core-sdk-integration.md](references/core-sdk-integration.md) when a
request involves Caelus Core, plugins, grants, or post-pairing control.

## Check prerequisites

For BLE provisioning require:

- A local Linux host with a powered Bluetooth adapter and BlueZ access.
- A saved open or PSK-based NetworkManager profile for the target 2.4 GHz
  network. Read its secret directly through `nmcli`; never ask the user to put
  a Wi-Fi password in chat or a command argument.
- The device powered on with pairing incomplete, advertising `Caelus-*`.
- The isolated BLE Python environment. If it is absent, explain that the setup
  downloads `bleak`, `cryptography`, and `protobuf`, obtain authorization, then
  run:

```bash
python3 <skill-dir>/scripts/setup_ble.py --yes
```

The default credential store is `~/.config/caelus/mcp-devices.json`. Honor
`CAELUS_DEVICE_STORE` or `--store`. The setup proof of possession defaults to
the development value compiled into this test-board firmware. Honor
`CAELUS_BLE_POP` for a per-device label value; never put that value in command
arguments, logs, or chat.

## Discover the setup device

Resolve `<skill-dir>` from this `SKILL.md`; do not assume the current working
directory. Scan without changing Wi-Fi:

```bash
python3 <skill-dir>/scripts/caelus_ble.py discover
```

Only accept devices whose name starts with `Caelus-`. Prefer a matching service
UUID as stronger discovery evidence, but tolerate platforms that omit UUIDs
from scan results and verify `caelus_pairing_v1` after connecting. If more than
one device is found, stop and ask the user to identify the printed name; never
select by signal strength alone.

## Provision over BLE

Run a dry run first. It resolves the saved NetworkManager credential source and
the exact BLE target but sends no secret:

```bash
python3 <skill-dir>/scripts/caelus_ble.py provision --dry-run
```

After the user authorizes transmission of the saved household Wi-Fi credential
to the local device, run:

```bash
python3 <skill-dir>/scripts/caelus_ble.py provision --yes
```

This flow keeps the host on its existing LAN and performs:

1. BLE discovery and exact-target selection.
2. Espressif Security 1 session establishment with proof of possession.
3. Standard `prov-config` Wi-Fi provisioning when the device has no saved
   credentials; an interrupted device with saved Wi-Fi skips that step.
4. A secure `custom-data` `pair` request that returns the device UID and MCP
   bearer credential.
5. Validation, atomic persistence, reload, and byte-for-byte comparison of the
   MCP connection record.
6. A secure `custom-data` `complete` request only after step 5 succeeds.
7. BLE/SoftAP shutdown on the device, followed by read-only LAN MCP manifest
   verification.

Use explicit selectors when necessary:

```bash
python3 <skill-dir>/scripts/caelus_ble.py provision --yes \
  --device Caelus-Board-A1B2C3 \
  --wifi-connection "Home WiFi" \
  --display-name "Desk lamp" \
  --hostname desk-lamp
```

When a saved 5 GHz profile shares credentials with a 2.4 GHz SSID, reuse it
only after the user confirms the credentials are shared:

```bash
python3 <skill-dir>/scripts/caelus_ble.py provision --yes \
  --wifi-connection "Home-5G" --wifi-ssid "Home"
```

Never infer shared credentials from an SSID suffix.

## Fall back to SoftAP recovery

Use SoftAP only when BLE is unavailable or the user explicitly chooses it. Warn
that a single-Wi-Fi host temporarily loses LAN connectivity:

```bash
python3 <skill-dir>/scripts/caelus_devices.py provision --dry-run
python3 <skill-dir>/scripts/caelus_devices.py provision --yes
```

The fallback joins `Caelus-*`, calls the portal APIs at `192.168.4.1`, restores
the original Wi-Fi in a cleanup path, and preserves the same store-before-
complete invariant. The browser and downloaded `.mcp.json` remain manual
recovery paths.

## Protect credentials

- Never expose a real MCP Token, Wi-Fi PSK, or production BLE proof of
  possession in source control, task summaries, chat, process arguments,
  screenshots, logs, or normal output.
- Keep the credential directory mode `0700` and registry mode `0600`; use
  atomic replacement.
- Persist and read back the MCP credential before sending `complete`. If local
  persistence fails, leave device pairing pending so the flow can resume.
- Treat BLE names, service advertisements, SoftAP names, and mDNS records as
  unauthenticated hints until the secure session and authenticated MCP
  manifest checks succeed.
- Use `show --reveal-token` only for an explicitly requested export. Require
  explicit confirmation for `remove --yes` and credential replacement.

## Verify and manage devices

Current firmware is native CMPP `1.0` over MCP `2025-11-25`. Verify
`cmpp.manifest.v1` from `cmpp://manifest`, matching `device_uid`/`device_id`,
the declared profile and capabilities, and read-only presence of
`cmpp.execute`. Never invoke an actuator as a pairing test.

```bash
python3 <skill-dir>/scripts/caelus_devices.py list
python3 <skill-dir>/scripts/caelus_devices.py tasks
python3 <skill-dir>/scripts/caelus_devices.py show <uid-or-name>
python3 <skill-dir>/scripts/caelus_devices.py discover
python3 <skill-dir>/scripts/caelus_devices.py verify <uid-or-name>
python3 <skill-dir>/scripts/caelus_devices.py remove <uid-or-name> --yes
```

For `caelus.test-board.v1`, require `test_board.pwm` and
`test_board.relay`. Accept `_caelus-hw._tcp.local` and
`caelus.hardware.manifest.v0` only for legacy firmware. Report native test-board
vendor attestation as `unavailable`; profile validation is not cryptographic
publisher attestation.

## Report outcomes separately

Always report: discovery transport, Pairing Task state, Wi-Fi provisioning
state, credential stored state, device pairing completion, authenticated
profile verification or pending reason, vendor attestation, plugin state
(`disabled` here), and capability grant (`not_granted` here).

For no BLE result, ask the user to power on/factory-reset the target and check
adapter access. For `401`, Authorization was missing; for `403`, the saved Token
was rejected. For `.local` delay, retain the safely stored credential and mark
verification pending. For an existing UID with a different Token, stop unless
the user confirms a factory reset and authorizes `--replace`.
