# Caelus Core SDK integration

Use this reference when a request mentions Caelus Core, the Pairing Plugin,
Board Connector, grants, or control after pairing.

## Version boundary

The current Core native Plugin SDK uses MCP `2026-07-28` between Core and a
signed local stdio Plugin. The ESP32 Board remains remote MCP `2025-11-25`, CMPP
`1.0`, with `cmpp.manifest.v1` at `cmpp://manifest`. These are separate protocol
legs. The Board Connector performs translation; never send a `2026-07-28`
remote request to the ESP32.

## Core-first decision

1. Query Core health and its signed action catalog. Require the exact Action ID
   and declared permission; do not infer an action from source types or UI text.
2. If an installed provisioning action explicitly supports the complete BLE or
   SoftAP transaction, use it and follow its arguments and permissions.
3. Otherwise use this Skill's BLE flow. Use `caelus_devices.py provision --yes`
   only as the SoftAP fallback after explicit network-switch authorization.
4. Configure the Pairing Plugin process with `CAELUS_DEVICE_STORE` pointing at
   the same private registry. It reads the protected registry and must never
   receive a Token in an action argument.
5. Use Core read actions to verify Connector readiness. A process being `ready`
   is not proof that a board is reachable; require `online: true`,
   `manifest_verified: true`, and `connector_state: ready` in board state.

## Grants and reconciliation

Pairing never grants device control. Request only the exact catalog-declared
permission immediately before each action. Core grants are ephemeral after
restart. `device.read` is enough for status/capabilities; physical commands
require the catalog's control permission.

When a physical action returns `unknown`, `reconcile_required`, or a Broker
transport failure, do not automatically retry. Restore the Connector/Broker and
perform a Core status read. An unavailable read leaves physical state
unresolved.
