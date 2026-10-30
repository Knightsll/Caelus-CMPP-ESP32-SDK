#!/usr/bin/env python3
"""Discover and provision Caelus devices over Espressif protocomm BLE."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
from pathlib import Path
import sys
import time
from types import ModuleType
from typing import Any
from urllib.parse import urlsplit


SCRIPT_DIR = Path(__file__).resolve().parent
VENDOR_DIR = SCRIPT_DIR / "vendor" / "esp_prov"
GENERATED_DIR = VENDOR_DIR / "proto" / "generated"
for path in (str(VENDOR_DIR), str(GENERATED_DIR), str(SCRIPT_DIR)):
    if path not in sys.path:
        sys.path.insert(0, path)

import caelus_devices as registry  # noqa: E402


BLE_SERVICE_UUID = "021a9004-0382-4aea-bff4-6b3f1c5adfb4"
BLE_NAME_PREFIX = "Caelus-"
DEFAULT_POP = "caelus1234"
DEFAULT_MQTT_TOPIC_PREFIX = "caelus/v1"


def _ble_venv_python() -> Path:
    configured = os.environ.get("CAELUS_BLE_VENV")
    root = Path(configured).expanduser() if configured else (
        Path.home() / ".cache" / "caelus-pair-device" / "venv"
    )
    return root / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


def _load_ble_stack() -> tuple[ModuleType, ModuleType, ModuleType, ModuleType]:
    try:
        import bleak
        import prov
        import security
        import transport
    except ImportError as exc:
        cached_python = _ble_venv_python()
        if cached_python.exists() and Path(sys.executable).resolve() != cached_python.resolve():
            os.execv(
                str(cached_python),
                [str(cached_python), str(Path(__file__).resolve()), *sys.argv[1:]],
            )
        raise registry.UserError(
            "BLE dependencies are missing; run scripts/setup_ble.py --yes once, then retry"
        ) from exc
    return bleak, prov, security, transport


def _ble_pop() -> str:
    value = os.environ.get("CAELUS_BLE_POP", DEFAULT_POP)
    if not 8 <= len(value.encode()) <= 64:
        raise registry.UserError("CAELUS_BLE_POP must contain 8 to 64 UTF-8 bytes")
    return value


async def scan_devices(timeout: float, adapter: str) -> list[dict[str, Any]]:
    bleak, _, _, _ = _load_ble_stack()
    kwargs: dict[str, Any] = {"timeout": timeout, "return_adv": True}
    if adapter:
        kwargs.update({"adapter": adapter, "bluez": {"adapter": adapter}})
    try:
        discovery = await bleak.BleakScanner.discover(**kwargs)
    except Exception as exc:
        raise registry.UserError(
            "Bluetooth scan failed; make sure the adapter is powered and accessible"
        ) from exc

    found: list[dict[str, Any]] = []
    for device, advertisement in discovery.values():
        name = device.name or advertisement.local_name or ""
        if not name.startswith(BLE_NAME_PREFIX):
            continue
        uuids = [str(item).casefold() for item in advertisement.service_uuids]
        found.append(
            {
                "name": name,
                "address": device.address,
                "rssi": advertisement.rssi,
                "service_uuid": BLE_SERVICE_UUID
                if BLE_SERVICE_UUID.casefold() in uuids
                else None,
            }
        )
    found.sort(key=lambda item: item["name"])
    return found


def _select_device(devices: list[dict[str, Any]], requested: str | None) -> dict[str, Any]:
    if requested:
        matches = [item for item in devices if item["name"] == requested]
        if not matches:
            raise registry.UserError(f"requested BLE device {requested!r} was not found")
        if len(matches) > 1:
            raise registry.UserError(
                "multiple BLE devices use that name; power down the unintended device and retry"
            )
        return matches[0]
    if not devices:
        raise registry.UserError(
            "no Caelus BLE setup service was found; power on or factory-reset the device"
        )
    if len(devices) > 1:
        names = ", ".join(item["name"] for item in devices)
        raise registry.UserError(
            f"multiple Caelus BLE devices were found ({names}); select one with --device"
        )
    return devices[0]


async def _capabilities(transport: Any) -> set[str]:
    raw = await transport.send_data("proto-ver", "none")
    try:
        value = json.loads(raw)
        capabilities = value["prov"]["cap"]
    except (TypeError, KeyError, json.JSONDecodeError) as exc:
        raise registry.UserError("device returned an invalid BLE capability document") from exc
    if not isinstance(capabilities, list):
        raise registry.UserError("device BLE capability list is invalid")
    return {str(item) for item in capabilities}


async def _establish_session(transport: Any, security_context: Any) -> None:
    response = None
    while True:
        message = security_context.security_session(response)
        if message is None:
            return
        response = await transport.send_data("prov-session", message)
        if response is None:
            raise registry.UserError("device ended the BLE security handshake")


async def _send_custom(transport: Any, security_context: Any,
                       payload: dict[str, Any]) -> dict[str, Any]:
    request_bytes = json.dumps(
        payload, ensure_ascii=False, separators=(",", ":")
    ).encode("utf-8")
    encrypted = security_context.encrypt_data(request_bytes).decode("latin-1")
    response = await transport.send_data("custom-data", encrypted)
    plaintext = security_context.decrypt_data(response.encode("latin-1"))
    try:
        value = json.loads(plaintext.rstrip(b"\x00").decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise registry.UserError("device returned an invalid BLE pairing response") from exc
    if not isinstance(value, dict):
        raise registry.UserError("device returned a non-object BLE pairing response")
    if not value.get("ok"):
        raise registry.UserError(
            f"device rejected BLE pairing: {value.get('error', 'unknown_error')}"
        )
    return value


async def _send_wifi(transport: Any, security_context: Any, prov: ModuleType,
                     ssid: str, password: str, timeout: float) -> None:
    message = prov.config_set_config_request("wifi", security_context, ssid, password)
    response = await transport.send_data("prov-config", message)
    if prov.config_set_config_response(security_context, response) != 0:
        raise registry.UserError("device rejected Wi-Fi credentials over BLE")

    message = prov.config_apply_config_request("wifi", security_context)
    response = await transport.send_data("prov-config", message)
    if prov.config_apply_config_response(security_context, response) != 0:
        raise registry.UserError("device could not apply Wi-Fi credentials")

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        await asyncio.sleep(1.0)
        message = prov.config_get_status_request("wifi", security_context)
        response = await transport.send_data("prov-config", message)
        state = prov.config_get_status_response(security_context, response)
        if state == "connected":
            return
        if state not in {"connecting", "disconnected"}:
            raise registry.UserError(f"device Wi-Fi provisioning failed ({state})")
    raise registry.UserError("device did not connect to Wi-Fi before the BLE timeout")


def _connection_from_ble(response: dict[str, Any], display_name: str,
                         hostname: str) -> dict[str, Any]:
    uid = response.get("device_uid")
    token = response.get("auth_token")
    return registry.validate_connection(
        {
            "schema": registry.CONNECTION_SCHEMA,
            "name": display_name,
            "device_uid": uid,
            "hostname": hostname,
            "transport": "streamable_http",
            "url": f"http://{hostname}.local:8080/mcp",
            "headers": {"Authorization": f"Bearer {token}"},
            "discovery": registry.NATIVE_DISCOVERY_SERVICE,
        }
    )


async def provision(args: argparse.Namespace) -> None:
    if not args.yes and not args.dry_run:
        raise registry.UserError(
            "BLE provisioning transmits the saved household Wi-Fi credential; rerun with --yes"
        )

    active_profiles = registry._active_wifi()
    selected = None
    if args.wifi_connection:
        selected = next(
            (item for item in active_profiles if item[0] == args.wifi_connection),
            (args.wifi_connection, ""),
        )
    elif active_profiles:
        selected = active_profiles[0]
    if not selected:
        raise registry.UserError(
            "no active household Wi-Fi profile was found; provide --wifi-connection"
        )
    source_ssid, password = registry._profile_credentials(selected[0])
    target_ssid = args.wifi_ssid or source_ssid

    devices = await scan_devices(args.scan_timeout, args.adapter)
    device = _select_device(devices, args.device)
    suffix = device["name"].rsplit("-", 1)[-1]
    display_name = args.display_name or f"Caelus Board {suffix}"
    hostname = args.hostname or f"caelus-board-{suffix.casefold()}"
    mqtt_config = None
    if args.mqtt_username and not args.mqtt_uri:
        raise registry.UserError("--mqtt-username requires --mqtt-uri")
    if args.mqtt_uri:
        parsed = urlsplit(args.mqtt_uri)
        if (
            parsed.scheme not in {"mqtt", "mqtts", "ws", "wss"}
            or not parsed.hostname
            or parsed.username
            or parsed.password
        ):
            raise registry.UserError(
                "--mqtt-uri must use mqtt, mqtts, ws, or wss, include a host, "
                "and keep credentials in separate options"
            )
        if (
            not args.mqtt_topic_prefix
            or args.mqtt_topic_prefix.startswith("/")
            or args.mqtt_topic_prefix.endswith("/")
            or any(value in args.mqtt_topic_prefix for value in "+#")
            or any(ord(value) <= 0x20 or ord(value) == 0x7F
                   for value in args.mqtt_topic_prefix)
        ):
            raise registry.UserError("--mqtt-topic-prefix is invalid")
        mqtt_password = os.environ.get(args.mqtt_password_env, "")
        limits = {
            "--mqtt-uri": (args.mqtt_uri, 192),
            "--mqtt-username": (args.mqtt_username or "", 64),
            "MQTT password": (mqtt_password, 128),
            "--mqtt-topic-prefix": (args.mqtt_topic_prefix, 64),
        }
        for label, (value, limit) in limits.items():
            if len(value.encode("utf-8")) >= limit:
                raise registry.UserError(f"{label} is too long")
        mqtt_config = {
            "uri": args.mqtt_uri,
            "username": args.mqtt_username or "",
            "password": mqtt_password,
            "topic_prefix": args.mqtt_topic_prefix,
        }
    if args.dry_run:
        print(
            json.dumps(
                {
                    "ok": True,
                    "dry_run": True,
                    "transport": "ble",
                    "device": device,
                    "wifi_connection": selected[0],
                    "wifi_ssid": target_ssid,
                    "display_name": display_name,
                    "hostname": hostname,
                    "mqtt": None if mqtt_config is None else {
                        "uri": mqtt_config["uri"],
                        "username": mqtt_config["username"],
                        "password_configured": bool(mqtt_config["password"]),
                        "topic_prefix": mqtt_config["topic_prefix"],
                    },
                    "credential_store": str(registry.resolve_store(args.store)),
                },
                ensure_ascii=False,
                indent=2,
            )
        )
        return

    store = registry.resolve_store(args.store)
    task_id, resumed = registry.start_pairing_task(
        store,
        device_ap=device["name"],
        interface=f"ble:{args.adapter}",
        household_connection=selected[0],
        household_ssid=target_ssid,
    )
    registry.update_pairing_task(store, task_id, "ble_connecting")
    connection = None
    transport_object = None
    try:
        _, prov, security, transport = _load_ble_stack()
        lookup = {
            "prov-session": "ff51",
            "prov-config": "ff52",
            "proto-ver": "ff53",
        }
        transport_object = transport.Transport_BLE(
            service_uuid=BLE_SERVICE_UUID,
            nu_lookup=lookup,
            ble_adapter=args.adapter,
        )
        await transport_object.connect(devname=device["name"])
        capabilities = await _capabilities(transport_object)
        if "caelus_pairing_v1" not in capabilities:
            raise registry.UserError("device does not advertise Caelus BLE pairing v1")

        security_context = security.Security1(_ble_pop(), False)
        await _establish_session(transport_object, security_context)
        if "wifi_prov" in capabilities:
            registry.update_pairing_task(store, task_id, "wifi_configuring")
            await _send_wifi(
                transport_object,
                security_context,
                prov,
                target_ssid,
                password,
                args.configure_timeout,
            )

        registry.update_pairing_task(store, task_id, "credential_receiving")
        pairing_request = {
            "action": "pair",
            "display_name": display_name,
            "hostname": hostname,
        }
        if mqtt_config is not None:
            pairing_request["mqtt"] = mqtt_config
        response = await _send_custom(
            transport_object, security_context, pairing_request
        )
        connection = _connection_from_ble(response, display_name, hostname)
        registry.update_pairing_task(
            store, task_id, "credential_received",
            device_uid=connection["device_uid"],
            credential_state="received_not_stored",
        )
        registry.save_connection_record(
            connection, store, args.replace, pairing_state="pending_device_ack"
        )
        registry.reload_connection_record(store, connection)
        registry.update_pairing_task(
            store, task_id, "credential_stored", credential_state="stored"
        )

        await _send_custom(
            transport_object, security_context, {"action": "complete"}
        )
        registry.update_pairing_task(
            store, task_id, "completed",
            device_uid=connection["device_uid"],
            credential_state="stored",
            verification_state="pending",
            household_wifi_restored=True,
        )
        saved_registry = registry.load_registry(store)
        _, saved_record = registry.select_record(
            saved_registry, connection["device_uid"]
        )
        saved_record["pairing"] = {
            "state": "completed",
            "completed_at": registry.utc_now(),
            "confirmed_by": "ble_pairing_ack",
        }
        saved_registry["updated_at"] = registry.utc_now()
        registry.save_registry(store, saved_registry)
    except BaseException as exc:
        state, reason = registry._failure_task_state(exc, connection)
        registry.update_pairing_task(store, task_id, state, reason=reason)
        raise
    finally:
        password = ""
        if transport_object is not None:
            await transport_object.disconnect()

    verification: dict[str, Any] = {"state": "pending"}
    if not args.skip_verify:
        try:
            await asyncio.sleep(args.settle_seconds)
            verification, _ = registry.verify_connection(
                connection, connection["device_uid"], args.verify_timeout
            )
            saved_registry = registry.load_registry(store)
            _, saved_record = registry.select_record(
                saved_registry, connection["device_uid"]
            )
            saved_record["verification"] = verification
            saved_record["pairing"] = {
                "state": "completed",
                "confirmed_by": "authenticated_manifest",
            }
            saved_registry["updated_at"] = registry.utc_now()
            registry.save_registry(store, saved_registry)
        except registry.UserError as exc:
            verification = {"state": "pending", "reason": str(exc)}
    print(
        json.dumps(
            {
                "ok": True,
                "transport": "ble",
                "pairing_task_id": task_id,
                "resumed_task": resumed,
                "device_uid": connection["device_uid"],
                "credential": "stored",
                "device_pairing": "completed",
                "mqtt_configured": bool(response.get("mqtt_configured")),
                "verification": verification,
                "vendor_attestation": "unavailable",
                "plugin": "disabled",
                "grant": "not_granted",
            },
            ensure_ascii=False,
            indent=2,
        )
    )


async def discover(args: argparse.Namespace) -> None:
    devices = await scan_devices(args.timeout, args.adapter)
    print(json.dumps({"transport": "ble", "devices": devices}, indent=2))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Discover and pair Caelus hardware over Bluetooth LE"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    discover_parser = subparsers.add_parser(
        "discover", help="discover Caelus-* BLE setup services"
    )
    discover_parser.add_argument("--adapter", default="hci0")
    discover_parser.add_argument("--timeout", type=float, default=8.0)
    discover_parser.set_defaults(handler=discover)

    provision_parser = subparsers.add_parser(
        "provision", help="provision Wi-Fi and persist MCP credentials over BLE"
    )
    provision_parser.add_argument("--device", help="exact Caelus-* BLE name")
    provision_parser.add_argument("--adapter", default="hci0")
    provision_parser.add_argument("--wifi-connection")
    provision_parser.add_argument("--wifi-ssid")
    provision_parser.add_argument("--display-name")
    provision_parser.add_argument("--hostname")
    provision_parser.add_argument(
        "--mqtt-uri", help="optional broker URI (prefer mqtts:// or wss://)"
    )
    provision_parser.add_argument("--mqtt-username")
    provision_parser.add_argument(
        "--mqtt-password-env", default="CAELUS_MQTT_PASSWORD",
        help="environment variable containing the broker password",
    )
    provision_parser.add_argument(
        "--mqtt-topic-prefix", default=DEFAULT_MQTT_TOPIC_PREFIX
    )
    provision_parser.add_argument("--store")
    provision_parser.add_argument("--replace", action="store_true")
    provision_parser.add_argument("--yes", action="store_true")
    provision_parser.add_argument("--dry-run", action="store_true")
    provision_parser.add_argument("--skip-verify", action="store_true")
    provision_parser.add_argument("--scan-timeout", type=float, default=8.0)
    provision_parser.add_argument("--configure-timeout", type=float, default=45.0)
    provision_parser.add_argument("--verify-timeout", type=float, default=15.0)
    provision_parser.add_argument("--settle-seconds", type=float, default=5.0)
    provision_parser.set_defaults(handler=provision)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    timeout_fields = {
        "timeout": (0.5, 60.0),
        "scan_timeout": (0.5, 60.0),
        "configure_timeout": (10.0, 120.0),
        "verify_timeout": (1.0, 60.0),
        "settle_seconds": (0.0, 60.0),
    }
    for field, limits in timeout_fields.items():
        if hasattr(args, field) and not limits[0] <= getattr(args, field) <= limits[1]:
            parser.error(
                f"--{field.replace('_', '-')} must be between {limits[0]:g} and {limits[1]:g} seconds"
            )
    try:
        asyncio.run(args.handler(args))
    except registry.UserError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("error: interrupted", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"error: BLE operation failed ({type(exc).__name__})", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
