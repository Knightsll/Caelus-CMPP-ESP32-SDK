#!/usr/bin/env python3
"""Secure registry and read-only verifier for official Caelus MCP devices."""

from __future__ import annotations

import argparse
import hashlib
import hmac
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from typing import Any
from urllib import error, parse, request


CONNECTION_SCHEMA = "caelus.mcp.connection.v1"
REGISTRY_SCHEMA = "caelus.mcp.device-registry.v1"
NATIVE_MANIFEST_SCHEMA = "cmpp.manifest.v1"
NATIVE_CMPP_PROTOCOL_VERSION = "1.0"
NATIVE_MANIFEST_URI = "cmpp://manifest"
NATIVE_EXECUTE_TOOL = "cmpp.execute"
NATIVE_DISCOVERY_SERVICE = "_cmpp._tcp.local"
NATIVE_TEST_BOARD_PROFILE = "caelus.test-board.v1"
NATIVE_TEST_BOARD_CAPABILITIES = {"test_board.pwm", "test_board.relay"}
LEGACY_MANIFEST_SCHEMA = "caelus.hardware.manifest.v0"
NATIVE_HARDWARE_PROFILE = "caelus.hardware.profile.v0"
LEGACY_DISCOVERY_SERVICE = "_caelus-hw._tcp.local"
DISCOVERY_SERVICES = {
    NATIVE_DISCOVERY_SERVICE: "_cmpp._tcp",
    LEGACY_DISCOVERY_SERVICE: "_caelus-hw._tcp",
}
MCP_PROTOCOL_VERSION = "2025-11-25"
LEGACY_HAP_PROTOCOL_VERSION = "0.1.0"
LEGACY_MANIFEST_URI = "caelus://hardware/manifest"
PORTAL_ORIGIN = "http://192.168.4.1"
SETUP_SSID_PREFIX = "Caelus-"
TEMP_PROFILE_PREFIX = "caelus-provision-"
MAX_JSON_BYTES = 1024 * 1024
TOKEN_RE = re.compile(r"Bearer ([0-9a-f]{64})\Z")
UID_RE = re.compile(r"hw_[A-Za-z0-9_-]{8,64}\Z")
HOSTNAME_RE = re.compile(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\Z")


class UserError(Exception):
    """Expected validation, lookup, storage, or network error."""


class NoRedirect(request.HTTPRedirectHandler):
    """Keep Bearer credentials from following redirects to another endpoint."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: ANN001
        return None


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def default_store_path() -> Path:
    configured = os.environ.get("CAELUS_DEVICE_STORE")
    if configured:
        return Path(configured).expanduser()
    return Path.home() / ".config" / "caelus" / "mcp-devices.json"


def resolve_store(value: str | None) -> Path:
    path = Path(value).expanduser() if value else default_store_path()
    if not path.is_absolute():
        path = (Path.cwd() / path).resolve()
    return path


def read_json_source(source: str) -> dict[str, Any]:
    if source == "-":
        raw = sys.stdin.buffer.read(MAX_JSON_BYTES + 1)
    else:
        path = Path(source).expanduser()
        if path.is_symlink():
            raise UserError("refusing to import a credential through a symbolic link")
        try:
            if path.stat().st_size > MAX_JSON_BYTES:
                raise UserError("connection file is too large")
            raw = path.read_bytes()
        except OSError as exc:
            raise UserError(f"cannot read connection file: {exc}") from exc
    if len(raw) > MAX_JSON_BYTES:
        raise UserError("connection file is too large")
    try:
        value = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise UserError(f"connection file is not valid UTF-8 JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise UserError("connection JSON must be an object")
    return value


def _contains_wifi_secret(value: Any) -> bool:
    if isinstance(value, dict):
        for key, item in value.items():
            if str(key).casefold() in {"ssid", "wifi_password", "wi-fi_password"}:
                return True
            if str(key).casefold() == "password" and str(key) != "Authorization":
                return True
            if _contains_wifi_secret(item):
                return True
    elif isinstance(value, list):
        return any(_contains_wifi_secret(item) for item in value)
    return False


def _valid_endpoint_host(host: str) -> bool:
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        return host.casefold().endswith(".local")
    return address.is_private or address.is_loopback or address.is_link_local


def validate_connection(value: dict[str, Any]) -> dict[str, Any]:
    if value.get("schema") != CONNECTION_SCHEMA:
        raise UserError(f"schema must be {CONNECTION_SCHEMA}")
    if _contains_wifi_secret(value):
        raise UserError("connection file must not contain Wi-Fi SSID/password data")

    name = value.get("name")
    uid = value.get("device_uid")
    hostname = value.get("hostname")
    transport = value.get("transport")
    endpoint = value.get("url")
    headers = value.get("headers")
    discovery = value.get("discovery")

    if not isinstance(name, str) or not name.strip() or len(name.encode()) >= 64:
        raise UserError("name is missing or too long")
    if not isinstance(uid, str) or not UID_RE.fullmatch(uid):
        raise UserError("device_uid is not a valid Caelus hardware UID")
    if not isinstance(hostname, str) or not HOSTNAME_RE.fullmatch(hostname):
        raise UserError("hostname is invalid")
    if transport != "streamable_http":
        raise UserError("transport must be streamable_http")
    if discovery not in DISCOVERY_SERVICES:
        supported = ", ".join(sorted(DISCOVERY_SERVICES))
        raise UserError(f"discovery must be one of: {supported}")
    if not isinstance(endpoint, str):
        raise UserError("url is missing")

    parsed = parse.urlsplit(endpoint)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        raise UserError("url must use HTTP(S) and include a host")
    if parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise UserError("url must not contain userinfo, query, or fragment")
    if parsed.path != "/mcp":
        raise UserError("url path must be /mcp")
    try:
        port = parsed.port
    except ValueError as exc:
        raise UserError("url port is invalid") from exc
    if port is not None and not 1 <= port <= 65535:
        raise UserError("url port is invalid")
    if not _valid_endpoint_host(parsed.hostname):
        raise UserError("url host must be .local or a private/link-local IP")
    if parsed.hostname.casefold().endswith(".local"):
        expected_host = f"{hostname}.local".casefold()
        if parsed.hostname.casefold() != expected_host:
            raise UserError("url hostname does not match the connection hostname")

    if not isinstance(headers, dict) or set(headers) != {"Authorization"}:
        raise UserError("headers must contain only Authorization")
    authorization = headers.get("Authorization")
    if not isinstance(authorization, str) or not TOKEN_RE.fullmatch(authorization):
        raise UserError("Authorization must contain a 64-hex-character Bearer Token")
    return value


def empty_registry() -> dict[str, Any]:
    return {
        "registry_schema": REGISTRY_SCHEMA,
        "devices": {},
        "pairing_tasks": {},
    }


def load_registry(path: Path) -> dict[str, Any]:
    if not path.exists():
        return empty_registry()
    if path.is_symlink():
        raise UserError("refusing to open a credential store symbolic link")
    try:
        if path.stat().st_size > MAX_JSON_BYTES:
            raise UserError("credential store is too large")
        mode = stat.S_IMODE(path.stat().st_mode)
        if mode & 0o077:
            os.chmod(path, 0o600)
        registry = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise UserError(f"cannot read credential store: {exc}") from exc
    if (
        not isinstance(registry, dict)
        or registry.get("registry_schema") != REGISTRY_SCHEMA
    ):
        raise UserError("credential store has an unsupported schema")
    if not isinstance(registry.get("devices"), dict):
        raise UserError("credential store devices field is invalid")
    pairing_tasks = registry.setdefault("pairing_tasks", {})
    if not isinstance(pairing_tasks, dict):
        raise UserError("credential store pairing_tasks field is invalid")
    return registry


def _ensure_private_parent(path: Path) -> None:
    parent = path.parent
    if parent.is_symlink():
        raise UserError("refusing to use a symbolic-link credential directory")
    if not parent.exists():
        try:
            parent.mkdir(parents=True, mode=0o700)
        except OSError as exc:
            raise UserError(f"cannot create credential directory: {exc}") from exc
    mode = stat.S_IMODE(parent.stat().st_mode)
    if mode & 0o077:
        if parent == default_store_path().parent:
            os.chmod(parent, 0o700)
        else:
            raise UserError(
                f"credential directory {parent} is accessible to other users; use a private 0700 directory"
            )


def save_registry(path: Path, registry: dict[str, Any]) -> None:
    _ensure_private_parent(path)
    if path.exists() and path.is_symlink():
        raise UserError("refusing to replace a credential store symbolic link")
    payload = (json.dumps(registry, ensure_ascii=False, indent=2) + "\n").encode()
    temporary: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix=f".{path.name}.", dir=path.parent, delete=False
        ) as handle:
            temporary = handle.name
            os.chmod(temporary, 0o600)
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        temporary = None
        os.chmod(path, 0o600)
    except OSError as exc:
        raise UserError(f"cannot save credential store: {exc}") from exc
    finally:
        if temporary:
            try:
                os.unlink(temporary)
            except OSError:
                pass


def _new_pairing_task_id() -> str:
    return f"pair_{secrets.token_hex(8)}"


def _pairing_task_matches(
    task: dict[str, Any],
    *,
    device_ap: str,
    interface: str,
    household_connection: str,
    household_ssid: str,
) -> bool:
    return (
        task.get("device_ap") == device_ap
        and task.get("interface") == interface
        and task.get("household_connection") == household_connection
        and task.get("household_ssid") == household_ssid
    )


def start_pairing_task(
    store: Path,
    *,
    device_ap: str,
    interface: str,
    household_connection: str,
    household_ssid: str,
) -> tuple[str, bool]:
    """Persist or resume the local Pairing Task before changing networks."""
    registry = load_registry(store)
    tasks = registry["pairing_tasks"]
    resumable_states = {
        "authorized",
        "network_switching",
        "setup_ap_connected",
        "credential_received",
        "credential_stored",
        "device_pairing_completed",
        "interrupted",
        "resume_required",
        "attention_required",
        "ble_connecting",
        "wifi_configuring",
        "credential_receiving",
    }
    matching = [
        task_id
        for task_id, task in tasks.items()
        if isinstance(task, dict)
        and task.get("state") in resumable_states
        and _pairing_task_matches(
            task,
            device_ap=device_ap,
            interface=interface,
            household_connection=household_connection,
            household_ssid=household_ssid,
        )
    ]
    if len(matching) > 1:
        raise UserError(
            "multiple resumable pairing tasks match this device; inspect the credential store before continuing"
        )

    now = utc_now()
    if matching:
        task_id = matching[0]
        task = tasks[task_id]
        task["state"] = "authorized"
        task["updated_at"] = now
        task["attempts"] = int(task.get("attempts", 1)) + 1
        task.pop("reason", None)
        resumed = True
    else:
        task_id = _new_pairing_task_id()
        tasks[task_id] = {
            "schema": "caelus.pairing.task.v1",
            "task_id": task_id,
            "state": "authorized",
            "created_at": now,
            "updated_at": now,
            "attempts": 1,
            "device_ap": device_ap,
            "interface": interface,
            "household_connection": household_connection,
            "household_ssid": household_ssid,
            "device_uid": None,
            "credential_state": "not_stored",
            "verification_state": "not_started",
        }
        resumed = False
    registry["updated_at"] = now
    save_registry(store, registry)
    return task_id, resumed


def update_pairing_task(store: Path, task_id: str, state: str, **fields: Any) -> None:
    registry = load_registry(store)
    task = registry["pairing_tasks"].get(task_id)
    if not isinstance(task, dict):
        raise UserError(f"pairing task {task_id} is missing from the credential store")
    task["state"] = state
    task["updated_at"] = utc_now()
    task.update(fields)
    registry["updated_at"] = task["updated_at"]
    save_registry(store, registry)


def recover_household_profile(store: Path, interface: str) -> str | None:
    """Recover the pre-switch profile after a process or host interruption."""
    registry = load_registry(store)
    terminal_states = {"completed", "failed", "cancelled"}
    candidates = {
        task.get("household_connection")
        for task in registry["pairing_tasks"].values()
        if isinstance(task, dict)
        and task.get("state") not in terminal_states
        and task.get("interface") == interface
        and isinstance(task.get("household_connection"), str)
        and not task["household_connection"].startswith(TEMP_PROFILE_PREFIX)
    }
    if len(candidates) > 1:
        raise UserError(
            "multiple interrupted pairing tasks name different household Wi-Fi profiles; provide --wifi-connection"
        )
    return next(iter(candidates), None)


def _failure_task_state(
    error_value: BaseException, connection: dict[str, Any] | None
) -> tuple[str, str]:
    if isinstance(error_value, KeyboardInterrupt):
        return "interrupted", "operator_interrupt"
    if connection is not None:
        return "resume_required", "credential_received_or_stored"
    if isinstance(error_value, UserError):
        return "failed", str(error_value)
    return "failed", type(error_value).__name__


def token_from(connection: dict[str, Any]) -> str:
    match = TOKEN_RE.fullmatch(connection["headers"]["Authorization"])
    if not match:  # validate_connection normally guarantees this.
        raise UserError("saved Authorization value is invalid")
    return match.group(1)


def masked_connection(connection: dict[str, Any]) -> dict[str, Any]:
    masked = json.loads(json.dumps(connection))
    token = token_from(connection)
    masked["headers"]["Authorization"] = f"Bearer {'*' * 12}{token[-6:]}"
    return masked


def select_record(
    registry: dict[str, Any], selector: str
) -> tuple[str, dict[str, Any]]:
    devices = registry["devices"]
    if selector in devices:
        return selector, devices[selector]
    folded = selector.casefold()
    matches = []
    for uid, record in devices.items():
        connection = record.get("connection", {})
        candidates = {
            str(connection.get("name", "")).casefold(),
            str(connection.get("hostname", "")).casefold(),
        }
        if folded in candidates:
            matches.append((uid, record))
    if not matches:
        raise UserError(f"no saved device matches {selector!r}")
    if len(matches) > 1:
        raise UserError("device name is ambiguous; select by device_uid")
    return matches[0]


def save_connection_record(
    connection: dict[str, Any],
    store: Path,
    replace: bool = False,
    pairing_state: str = "unknown",
) -> tuple[dict[str, Any], dict[str, Any]]:
    connection = validate_connection(connection)
    registry = load_registry(store)
    uid = connection["device_uid"]
    existing = registry["devices"].get(uid)
    if existing:
        old = token_from(validate_connection(existing["connection"]))
        new = token_from(connection)
        if not hmac.compare_digest(old, new) and not replace:
            raise UserError(
                "this device_uid already has a different Token; confirm factory reset, then use --replace"
            )
        verification = existing.get("verification", {"state": "unverified"})
        if not hmac.compare_digest(old, new):
            verification = {"state": "unverified", "reason": "credential_replaced"}
        plugin = existing.get("plugin", {"state": "disabled"})
        grant = existing.get("grant", {"state": "not_granted"})
    else:
        verification = {"state": "unverified"}
        plugin = {"state": "disabled"}
        grant = {"state": "not_granted"}
    saved_at = utc_now()
    contract, _, _ = _connection_contract(connection)
    record = {
        "connection": connection,
        "contract": contract,
        "saved_at": saved_at,
        "pairing": {"state": pairing_state},
        "credential": {"state": "stored", "stored_at": saved_at},
        "verification": verification,
        "plugin": plugin,
        "grant": grant,
    }
    registry["devices"][uid] = record
    registry["updated_at"] = utc_now()
    save_registry(store, registry)
    return registry, record


def reload_connection_record(
    store: Path, expected_connection: dict[str, Any]
) -> dict[str, Any]:
    """Reload and compare a credential after atomic persistence."""
    expected = validate_connection(expected_connection)
    registry = load_registry(store)
    _, record = select_record(registry, expected["device_uid"])
    stored = validate_connection(record.get("connection", {}))
    expected_bytes = json.dumps(
        expected, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode()
    stored_bytes = json.dumps(
        stored, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode()
    if not hmac.compare_digest(expected_bytes, stored_bytes):
        raise UserError("credential read-back did not match the portal response")
    credential = record.get("credential")
    if not isinstance(credential, dict) or credential.get("state") != "stored":
        raise UserError("credential record did not persist the stored state")
    return stored


def import_connection(args: argparse.Namespace) -> None:
    connection = validate_connection(read_json_source(args.file))
    store = resolve_store(args.store)
    _, record = save_connection_record(
        connection, store, args.replace, pairing_state="unknown"
    )
    verification = record["verification"]
    print(
        json.dumps(
            {
                "ok": True,
                "device_uid": connection["device_uid"],
                "name": connection["name"],
                "url": connection["url"],
                "contract": record["contract"],
                "credential_store": str(store),
                "credential": record["credential"]["state"],
                "verification": verification.get("state", "unverified"),
                "plugin": record["plugin"]["state"],
                "grant": record["grant"]["state"],
            },
            ensure_ascii=False,
            indent=2,
        )
    )


def list_devices(args: argparse.Namespace) -> None:
    registry = load_registry(resolve_store(args.store))
    output = []
    for uid, record in sorted(registry["devices"].items()):
        connection = record.get("connection", {})
        verification = record.get("verification", {})
        credential = record.get("credential", {})
        plugin = record.get("plugin", {})
        grant = record.get("grant", {})
        output.append(
            {
                "device_uid": uid,
                "name": connection.get("name"),
                "hostname": connection.get("hostname"),
                "url": connection.get("url"),
                "contract": record.get("contract")
                or _connection_contract(validate_connection(connection))[0],
                "credential": credential.get("state", "unknown"),
                "verification": verification.get("state", "unverified"),
                "verified_at": verification.get("verified_at"),
                "plugin": plugin.get("state", "disabled"),
                "grant": grant.get("state", "not_granted"),
            }
        )
    print(json.dumps({"devices": output}, ensure_ascii=False, indent=2))


def show_device(args: argparse.Namespace) -> None:
    registry = load_registry(resolve_store(args.store))
    _, record = select_record(registry, args.selector)
    output = json.loads(json.dumps(record))
    output.setdefault("credential", {"state": "stored"})
    output.setdefault("plugin", {"state": "disabled"})
    output.setdefault("grant", {"state": "not_granted"})
    if not args.reveal_token:
        output["connection"] = masked_connection(output["connection"])
    print(json.dumps(output, ensure_ascii=False, indent=2))


def list_pairing_tasks(args: argparse.Namespace) -> None:
    registry = load_registry(resolve_store(args.store))
    output = []
    for task_id, task in sorted(registry["pairing_tasks"].items()):
        if not isinstance(task, dict):
            continue
        output.append(
            {
                "task_id": task_id,
                "state": task.get("state"),
                "device_ap": task.get("device_ap"),
                "device_uid": task.get("device_uid"),
                "interface": task.get("interface"),
                "household_connection": task.get("household_connection"),
                "attempts": task.get("attempts"),
                "credential": task.get("credential_state"),
                "verification": task.get("verification_state"),
                "household_wifi_restored": task.get("household_wifi_restored"),
                "reason": task.get("reason"),
                "updated_at": task.get("updated_at"),
            }
        )
    print(json.dumps({"pairing_tasks": output}, ensure_ascii=False, indent=2))


def _parse_mcp_body(raw: bytes) -> dict[str, Any] | None:
    if not raw.strip():
        return None
    if len(raw) > MAX_JSON_BYTES:
        raise UserError("MCP response is too large")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise UserError("MCP response is not UTF-8") from exc
    candidates = []
    stripped = text.strip()
    if stripped.startswith("{"):
        candidates.append(stripped)
    else:
        for line in text.splitlines():
            if line.startswith("data:"):
                candidates.append(line[5:].strip())
    for candidate in candidates:
        try:
            value = json.loads(candidate)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            return value
    raise UserError("MCP response did not contain a JSON-RPC object")


def _mcp_post(
    connection: dict[str, Any],
    payload: dict[str, Any],
    timeout: float,
    session_id: str | None = None,
) -> tuple[dict[str, Any] | None, str | None]:
    headers = dict(connection["headers"])
    headers.update(
        {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        }
    )
    if session_id:
        headers["MCP-Session-Id"] = session_id
        headers["MCP-Protocol-Version"] = MCP_PROTOCOL_VERSION
    body = json.dumps(payload, separators=(",", ":")).encode()
    req = request.Request(connection["url"], data=body, headers=headers, method="POST")
    opener = request.build_opener(request.ProxyHandler({}), NoRedirect())
    try:
        with opener.open(req, timeout=timeout) as response:
            raw = response.read(MAX_JSON_BYTES + 1)
            returned_session = response.headers.get("MCP-Session-Id") or session_id
    except error.HTTPError as exc:
        if exc.code == 401:
            raise UserError("MCP authentication missing (HTTP 401)") from exc
        if exc.code == 403:
            raise UserError("MCP Token rejected (HTTP 403)") from exc
        if 300 <= exc.code < 400:
            raise UserError(
                "MCP endpoint attempted a redirect; refusing to forward the Token"
            ) from exc
        raise UserError(f"MCP endpoint returned HTTP {exc.code}") from exc
    except (error.URLError, TimeoutError, OSError) as exc:
        raise UserError(f"cannot reach MCP endpoint: {exc}") from exc
    return _parse_mcp_body(raw), returned_session


def _rpc_result(message: dict[str, Any] | None, request_id: int) -> dict[str, Any]:
    if not isinstance(message, dict):
        raise UserError("MCP response is empty")
    if message.get("id") != request_id:
        raise UserError("MCP response id does not match the request")
    if "error" in message:
        error_value = message.get("error")
        code = error_value.get("code") if isinstance(error_value, dict) else "unknown"
        raise UserError(f"MCP JSON-RPC error {code}")
    result = message.get("result")
    if not isinstance(result, dict):
        raise UserError("MCP response has no result object")
    return result


def _manifest_from_result(result: dict[str, Any]) -> dict[str, Any]:
    contents = result.get("contents")
    if not isinstance(contents, list) or not contents:
        raise UserError("manifest resource is empty")
    text = contents[0].get("text") if isinstance(contents[0], dict) else None
    if not isinstance(text, str):
        raise UserError("manifest resource is not textual JSON")
    try:
        manifest = json.loads(text)
    except json.JSONDecodeError as exc:
        raise UserError("manifest resource is not valid JSON") from exc
    if not isinstance(manifest, dict):
        raise UserError("manifest must be a JSON object")
    return manifest


def _connection_contract(connection: dict[str, Any]) -> tuple[str, str, str | None]:
    discovery = connection["discovery"]
    if discovery == NATIVE_DISCOVERY_SERVICE:
        return "native_cmpp_v1", NATIVE_MANIFEST_URI, NATIVE_EXECUTE_TOOL
    return "legacy_hardware_v0", LEGACY_MANIFEST_URI, None


def _validate_native_manifest(
    manifest: dict[str, Any], uid: str
) -> dict[str, Any]:
    if manifest.get("schema") != NATIVE_MANIFEST_SCHEMA:
        raise UserError(f"manifest schema is not {NATIVE_MANIFEST_SCHEMA}")
    if manifest.get("protocol_version") != NATIVE_CMPP_PROTOCOL_VERSION:
        raise UserError(
            f"manifest protocol_version is not {NATIVE_CMPP_PROTOCOL_VERSION}"
        )
    if manifest.get("device_id") != uid:
        raise UserError("manifest device_id does not match the saved device_uid")
    profile_id = manifest.get("profile_id")
    if not isinstance(profile_id, str) or not profile_id.strip():
        raise UserError("manifest profile_id is missing")
    state_version = manifest.get("state_version")
    if (
        not isinstance(state_version, int)
        or isinstance(state_version, bool)
        or state_version < 0
    ):
        raise UserError("manifest state_version is invalid")
    capabilities = manifest.get("capabilities")
    if not isinstance(capabilities, list) or not capabilities:
        raise UserError("manifest capabilities field is invalid or empty")

    capability_ids: list[str] = []
    for capability in capabilities:
        if not isinstance(capability, dict):
            raise UserError("manifest capability entry is invalid")
        capability_id = capability.get("capability_id")
        command_types = capability.get("command_types")
        if not isinstance(capability_id, str) or not capability_id:
            raise UserError("manifest capability_id is invalid")
        if capability_id in capability_ids:
            raise UserError("manifest contains a duplicate capability_id")
        if (
            not isinstance(command_types, list)
            or not command_types
            or not all(isinstance(item, str) and item for item in command_types)
        ):
            raise UserError(f"manifest command_types are invalid for {capability_id}")
        if not isinstance(capability.get("version"), str):
            raise UserError(f"manifest capability version is invalid for {capability_id}")
        allowed_safety_mask = capability.get("allowed_safety_mask")
        if not isinstance(allowed_safety_mask, int) or isinstance(
            allowed_safety_mask, bool
        ):
            raise UserError(
                f"manifest allowed_safety_mask is invalid for {capability_id}"
            )
        if not isinstance(capability.get("requires_ack"), bool):
            raise UserError(f"manifest requires_ack is invalid for {capability_id}")
        if not isinstance(capability.get("supports_dry_run"), bool):
            raise UserError(
                f"manifest supports_dry_run is invalid for {capability_id}"
            )
        capability_ids.append(capability_id)

    if profile_id == NATIVE_TEST_BOARD_PROFILE:
        missing = sorted(NATIVE_TEST_BOARD_CAPABILITIES - set(capability_ids))
        if missing:
            raise UserError(
                "test-board manifest is missing capabilities: " + ", ".join(missing)
            )
    return {
        "contract": "native_cmpp_v1",
        "profile_id": profile_id,
        "capability_ids": sorted(capability_ids),
        "official_device": False,
        "vendor_attestation": "unavailable",
    }


def _validate_legacy_manifest(
    manifest: dict[str, Any], uid: str, connection: dict[str, Any] | None
) -> dict[str, Any]:
    if manifest.get("schema") != LEGACY_MANIFEST_SCHEMA:
        raise UserError(f"manifest schema is not {LEGACY_MANIFEST_SCHEMA}")
    if manifest.get("protocol_version") != LEGACY_HAP_PROTOCOL_VERSION:
        raise UserError(
            f"manifest protocol_version is not {LEGACY_HAP_PROTOCOL_VERSION}"
        )
    if manifest.get("vendor") != "Caelus":
        raise UserError("manifest vendor is not Caelus")
    if manifest.get("device_uid") != uid:
        raise UserError("manifest device_uid does not match the saved credential")
    pairing = manifest.get("pairing")
    if not isinstance(pairing, dict) or pairing.get("state") != "paired":
        raise UserError("manifest does not report a paired device")
    required = ("transports", "capabilities", "commands", "cloud_egress", "privacy")
    missing = [field for field in required if field not in manifest]
    if missing:
        raise UserError(f"manifest is missing required fields: {', '.join(missing)}")
    if not isinstance(manifest["transports"], list):
        raise UserError("manifest transports field is invalid")
    if not isinstance(manifest["capabilities"], list):
        raise UserError("manifest capabilities field is invalid")
    if not isinstance(manifest["commands"], list):
        raise UserError("manifest commands field is invalid")
    compatibility = manifest.get("x_cmpp_compatibility")
    expected_compatibility = {
        "surface": "legacy_mcp",
        "mcp_version": MCP_PROTOCOL_VERSION,
        "legacy_connection_schema": CONNECTION_SCHEMA,
        "native_target": NATIVE_HARDWARE_PROFILE,
        "connector_required": True,
    }
    if not isinstance(compatibility, dict) or any(
        compatibility.get(key) != value
        for key, value in expected_compatibility.items()
    ):
        raise UserError("manifest does not declare the required legacy-to-CMPP mapping")
    if connection is not None:
        connection = validate_connection(connection)
        expected_encryption = parse.urlsplit(connection["url"]).scheme == "https"
        matching_transport = any(
            isinstance(item, dict)
            and item.get("type") == connection["transport"]
            and item.get("endpoint") == connection["url"]
            and item.get("encrypted") is expected_encryption
            for item in manifest["transports"]
        )
        if not matching_transport:
            raise UserError("manifest transport does not match the saved connection")
    capability_ids = sorted(
        item.get("capability_id")
        for item in manifest["capabilities"]
        if isinstance(item, dict) and isinstance(item.get("capability_id"), str)
    )
    return {
        "contract": "legacy_hardware_v0",
        "profile_id": NATIVE_HARDWARE_PROFILE,
        "capability_ids": capability_ids,
        "official_device": True,
        "vendor_attestation": "legacy_manifest_vendor_caelus",
    }


def validate_manifest(
    manifest: dict[str, Any],
    uid: str,
    connection: dict[str, Any] | None = None,
) -> dict[str, Any]:
    if connection is not None:
        connection = validate_connection(connection)
        contract, _, _ = _connection_contract(connection)
    else:
        contract = (
            "native_cmpp_v1"
            if manifest.get("schema") == NATIVE_MANIFEST_SCHEMA
            else "legacy_hardware_v0"
        )
    if contract == "native_cmpp_v1":
        return _validate_native_manifest(manifest, uid)
    return _validate_legacy_manifest(manifest, uid, connection)


def verify_connection(
    connection: dict[str, Any],
    uid: str,
    timeout: float,
) -> tuple[dict[str, Any], dict[str, Any]]:
    connection = validate_connection(connection)
    contract, manifest_uri, required_tool = _connection_contract(connection)
    initialize = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": MCP_PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "caelus-pair-device", "version": "1.0.0"},
        },
    }
    message, session = _mcp_post(connection, initialize, timeout)
    init_result = _rpc_result(message, 1)
    if init_result.get("protocolVersion") != MCP_PROTOCOL_VERSION:
        raise UserError(
            f"MCP server did not negotiate protocol {MCP_PROTOCOL_VERSION}"
        )
    if not session:
        raise UserError("MCP initialize response did not include MCP-Session-Id")

    notification = {
        "jsonrpc": "2.0",
        "method": "notifications/initialized",
        "params": {},
    }
    _mcp_post(connection, notification, timeout, session)

    tools_request = {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}
    message, _ = _mcp_post(connection, tools_request, timeout, session)
    tools_result = _rpc_result(message, 2)
    tools = tools_result.get("tools")
    if not isinstance(tools, list):
        raise UserError("MCP tools/list response is invalid")
    tool_names = sorted(
        tool.get("name")
        for tool in tools
        if isinstance(tool, dict) and isinstance(tool.get("name"), str)
    )
    if required_tool and required_tool not in tool_names:
        raise UserError(f"MCP tools/list is missing required tool {required_tool}")

    resources_request = {
        "jsonrpc": "2.0",
        "id": 3,
        "method": "resources/list",
        "params": {},
    }
    message, _ = _mcp_post(connection, resources_request, timeout, session)
    resources_result = _rpc_result(message, 3)
    resources = resources_result.get("resources")
    if not isinstance(resources, list):
        raise UserError("MCP resources/list response is invalid")
    resource_uris = sorted(
        resource.get("uri")
        for resource in resources
        if isinstance(resource, dict) and isinstance(resource.get("uri"), str)
    )
    if manifest_uri not in resource_uris:
        raise UserError(f"MCP resources/list is missing {manifest_uri}")

    manifest_request = {
        "jsonrpc": "2.0",
        "id": 4,
        "method": "resources/read",
        "params": {"uri": manifest_uri},
    }
    message, _ = _mcp_post(connection, manifest_request, timeout, session)
    manifest = _manifest_from_result(_rpc_result(message, 4))
    manifest_details = validate_manifest(manifest, uid, connection)

    canonical_manifest = json.dumps(
        manifest, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode()
    verification = {
        "state": "verified",
        "verified_at": utc_now(),
        "manifest_sha256": hashlib.sha256(canonical_manifest).hexdigest(),
        "manifest": manifest,
        "server_info": init_result.get("serverInfo", {}),
        "tools": tool_names,
        "resources": resource_uris,
        "contract": contract,
        "manifest_uri": manifest_uri,
        "vendor_attestation": manifest_details["vendor_attestation"],
    }
    summary = {
        "ok": True,
        "device_uid": uid,
        "name": connection["name"],
        "official_device": manifest_details["official_device"],
        "device_profile_verified": True,
        "vendor_attestation": manifest_details["vendor_attestation"],
        "contract": contract,
        "manifest_schema": manifest["schema"],
        "manifest_uri": manifest_uri,
        "protocol_version": manifest.get("protocol_version"),
        "profile_id": manifest_details["profile_id"],
        "capabilities": manifest_details["capability_ids"],
        "model": manifest.get("model"),
        "firmware_version": manifest.get("firmware_version"),
        "tools": tool_names,
        "credential": "stored",
        "verification": "verified",
        "plugin": "disabled",
        "grant": "not_granted",
        "capability_enabled": False,
    }
    return verification, summary


def verify_device(args: argparse.Namespace) -> None:
    store = resolve_store(args.store)
    registry = load_registry(store)
    uid, record = select_record(registry, args.selector)
    connection = validate_connection(record["connection"])
    verification, summary = verify_connection(connection, uid, args.timeout)
    record["pairing"] = {
        "state": "completed",
        "confirmed_by": "authenticated_manifest",
    }
    record.setdefault(
        "credential", {"state": "stored", "stored_at": record.get("saved_at")}
    )
    record["verification"] = verification
    record.setdefault("plugin", {"state": "disabled"})
    record.setdefault("grant", {"state": "not_granted"})
    registry["updated_at"] = utc_now()
    save_registry(store, registry)
    print(json.dumps(summary, ensure_ascii=False, indent=2))


def _nmcli_fields(line: str) -> list[str]:
    fields: list[str] = []
    current: list[str] = []
    escaped = False
    for character in line:
        if escaped:
            current.append(character)
            escaped = False
        elif character == "\\":
            escaped = True
        elif character == ":":
            fields.append("".join(current))
            current = []
        else:
            current.append(character)
    if escaped:
        current.append("\\")
    fields.append("".join(current))
    return fields


def _run_nmcli(arguments: list[str], timeout: float = 30.0) -> str:
    executable = shutil.which("nmcli")
    if not executable:
        raise UserError("nmcli is required for unattended provisioning")
    try:
        result = subprocess.run(
            [executable, *arguments],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise UserError("NetworkManager operation timed out") from exc
    if result.returncode != 0:
        raise UserError(
            f"NetworkManager operation failed (exit {result.returncode}); check Wi-Fi/Polkit permissions"
        )
    return result.stdout.rstrip("\n")


def _active_wifi() -> list[tuple[str, str]]:
    output = _run_nmcli(
        [
            "-t",
            "--escape",
            "yes",
            "-f",
            "NAME,TYPE,DEVICE",
            "connection",
            "show",
            "--active",
        ]
    )
    profiles = []
    for line in output.splitlines():
        fields = _nmcli_fields(line)
        if len(fields) >= 3 and fields[1] in {"802-11-wireless", "wifi"}:
            profiles.append((fields[0], fields[2]))
    return profiles


def _first_wifi_device() -> str:
    output = _run_nmcli(
        [
            "-t",
            "--escape",
            "yes",
            "-f",
            "DEVICE,TYPE",
            "device",
            "status",
        ]
    )
    for line in output.splitlines():
        fields = _nmcli_fields(line)
        if len(fields) >= 2 and fields[1] in {"wifi", "802-11-wireless"}:
            return fields[0]
    raise UserError("NetworkManager did not report a Wi-Fi interface")


def _profile_property(profile: str, property_name: str, secret: bool = False) -> str:
    arguments = []
    if secret:
        arguments.append("--show-secrets")
    arguments.extend(
        [
            "--escape",
            "no",
            "-g",
            property_name,
            "connection",
            "show",
            profile,
        ]
    )
    return _run_nmcli(arguments).strip()


def _profile_credentials(profile: str) -> tuple[str, str]:
    ssid = _profile_property(profile, "802-11-wireless.ssid")
    if not ssid:
        raise UserError("the selected NetworkManager profile has no Wi-Fi SSID")
    key_management = _profile_property(
        profile, "802-11-wireless-security.key-mgmt"
    ).casefold()
    if key_management in {"", "--", "none"}:
        return ssid, ""
    if key_management not in {"wpa-psk", "sae", "wpa-psk,sae", "sae,wpa-psk"}:
        raise UserError(
            f"unsupported household Wi-Fi authentication {key_management!r}; use a PSK/open 2.4 GHz profile"
        )
    password = _profile_property(profile, "802-11-wireless-security.psk", secret=True)
    if not 8 <= len(password.encode()) <= 64:
        raise UserError(
            "NetworkManager did not provide a valid saved PSK; grant secret-read permission first"
        )
    return ssid, password


def _scan_setup_aps(interface: str) -> list[str]:
    output = _run_nmcli(
        [
            "-t",
            "--escape",
            "yes",
            "-f",
            "SSID",
            "device",
            "wifi",
            "list",
            "--rescan",
            "yes",
            "ifname",
            interface,
        ],
        timeout=45.0,
    )
    found: list[str] = []
    for line in output.splitlines():
        if not line:
            continue
        ssid = _nmcli_fields(line)[0]
        if ssid.startswith(SETUP_SSID_PREFIX) and ssid not in found:
            found.append(ssid)
    return found


def _select_setup_ap(interface: str, requested: str | None) -> str:
    found = _scan_setup_aps(interface)
    if requested:
        if not requested.startswith(SETUP_SSID_PREFIX):
            raise UserError(f"setup SSID must start with {SETUP_SSID_PREFIX}")
        if requested not in found:
            raise UserError(f"requested setup hotspot {requested!r} was not found")
        return requested
    if not found:
        raise UserError("no Caelus setup hotspot was found")
    if len(found) > 1:
        raise UserError(
            "multiple Caelus setup hotspots were found; select one with --device-ap"
        )
    return found[0]


def _portal_raw(
    path: str,
    method: str = "GET",
    payload: dict[str, Any] | None = None,
    timeout: float = 10.0,
    allow_404: bool = False,
) -> bytes | None:
    url = f"{PORTAL_ORIGIN}{path}"
    headers: dict[str, str] = {"Cache-Control": "no-store"}
    data = None
    if payload is not None:
        data = json.dumps(payload, separators=(",", ":")).encode()
        headers["Content-Type"] = "application/json"
    req = request.Request(url, data=data, headers=headers, method=method)
    opener = request.build_opener(request.ProxyHandler({}), NoRedirect())
    try:
        with opener.open(req, timeout=timeout) as response:
            raw = response.read(MAX_JSON_BYTES + 1)
    except error.HTTPError as exc:
        if allow_404 and exc.code == 404:
            return None
        raise UserError(f"setup portal returned HTTP {exc.code} for {path}") from exc
    except (error.URLError, TimeoutError, OSError) as exc:
        raise UserError(f"cannot reach setup portal {PORTAL_ORIGIN}: {exc}") from exc
    if len(raw) > MAX_JSON_BYTES:
        raise UserError("setup portal response is too large")
    return raw


def _portal_json(
    path: str,
    method: str = "GET",
    payload: dict[str, Any] | None = None,
    timeout: float = 10.0,
    allow_404: bool = False,
) -> dict[str, Any] | None:
    raw = _portal_raw(path, method, payload, timeout, allow_404)
    if raw is None:
        return None
    try:
        value = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise UserError(f"setup portal returned invalid JSON for {path}") from exc
    if not isinstance(value, dict):
        raise UserError(f"setup portal returned a non-object for {path}")
    if not value.get("ok"):
        code = value.get("error", "unknown_error")
        raise UserError(f"setup portal rejected {path}: {code}")
    return value


def _wait_for_portal(timeout: float) -> None:
    deadline = time.monotonic() + timeout
    last_error: UserError | None = None
    while time.monotonic() < deadline:
        try:
            _portal_raw("/", timeout=min(3.0, timeout))
            return
        except UserError as exc:
            last_error = exc
            time.sleep(0.5)
    raise UserError(f"setup portal did not become ready: {last_error}")


def _connection_from_portal(value: dict[str, Any]) -> dict[str, Any]:
    device = value.get("device")
    connection = value.get("connection")
    if not isinstance(device, dict) or not isinstance(connection, dict):
        raise UserError("setup portal did not return device/connection objects")
    bundle = {
        "schema": CONNECTION_SCHEMA,
        "name": device.get("display_name"),
        "device_uid": device.get("device_uid"),
        "hostname": device.get("hostname"),
        "transport": connection.get("transport"),
        "url": connection.get("url"),
        "headers": connection.get("headers"),
        "discovery": device.get("discovery"),
    }
    return validate_connection(bundle)


def provision_device(args: argparse.Namespace) -> None:
    if not args.yes and not args.dry_run:
        raise UserError(
            "unattended provisioning changes the host Wi-Fi and transmits its saved PSK; rerun with --yes"
        )

    store = resolve_store(args.store)
    active_profiles = _active_wifi()
    stale_temporary_profiles = [
        item for item in active_profiles if item[0].startswith(TEMP_PROFILE_PREFIX)
    ]
    household_active_profiles = [
        item for item in active_profiles if not item[0].startswith(TEMP_PROFILE_PREFIX)
    ]
    selected_active = None
    if args.interface:
        selected_active = next(
            (
                item
                for item in household_active_profiles
                if item[1] == args.interface
            ),
            None,
        )
    elif args.wifi_connection:
        selected_active = next(
            (
                item
                for item in household_active_profiles
                if item[0] == args.wifi_connection
            ),
            None,
        )
    elif len(household_active_profiles) == 1:
        selected_active = household_active_profiles[0]
    elif len(household_active_profiles) > 1:
        raise UserError(
            "multiple household Wi-Fi connections are active; provide --interface or --wifi-connection"
        )
    interface = args.interface or (
        selected_active[1]
        if selected_active
        else (
            stale_temporary_profiles[0][1]
            if len(stale_temporary_profiles) == 1
            else _first_wifi_device()
        )
    )
    home_profile = args.wifi_connection or (
        selected_active[0] if selected_active else None
    )
    recovered_home_profile = False
    if not home_profile and stale_temporary_profiles:
        home_profile = recover_household_profile(store, interface)
        recovered_home_profile = home_profile is not None
    if not home_profile:
        raise UserError(
            "no active household Wi-Fi profile; provide --wifi-connection and optionally --interface"
        )
    credential_source_ssid, home_password = _profile_credentials(home_profile)
    home_ssid = args.wifi_ssid or credential_source_ssid
    if not 1 <= len(home_ssid.encode("utf-8")) <= 32:
        raise UserError("the target household Wi-Fi SSID must be 1 to 32 UTF-8 bytes")
    setup_ap = _select_setup_ap(interface, args.device_ap)

    if args.dry_run:
        print(
            json.dumps(
                {
                    "ok": True,
                    "dry_run": True,
                    "headless": True,
                    "interface": interface,
                    "device_ap": setup_ap,
                    "household_connection": home_profile,
                    "household_ssid": home_ssid,
                    "credential_source_ssid": credential_source_ssid,
                    "credential_store": str(store),
                    "recovered_interrupted_task": recovered_home_profile,
                    "stale_temporary_profiles": [
                        name for name, _ in stale_temporary_profiles
                    ],
                    "household_authentication": "open" if not home_password else "psk",
                    "network_credential_ready": True,
                },
                ensure_ascii=False,
                indent=2,
            )
        )
        return

    task_id, task_resumed = start_pairing_task(
        store,
        device_ap=setup_ap,
        interface=interface,
        household_connection=home_profile,
        household_ssid=home_ssid,
    )
    temporary_profile = f"caelus-provision-{os.getpid()}-{int(time.monotonic() * 1000)}"
    switched = False
    workflow_error: BaseException | None = None
    restore_error: UserError | None = None
    connection: dict[str, Any] | None = None
    resumed = False
    paired = False
    try:
        update_pairing_task(store, task_id, "network_switching")
        _run_nmcli(
            [
                "connection",
                "add",
                "type",
                "wifi",
                "ifname",
                interface,
                "con-name",
                temporary_profile,
                "ssid",
                setup_ap,
            ]
        )
        _run_nmcli(
            ["connection", "modify", temporary_profile, "connection.autoconnect", "no"]
        )
        switched = True
        _run_nmcli(["connection", "up", temporary_profile, "ifname", interface], 45.0)
        _wait_for_portal(args.portal_timeout)
        update_pairing_task(store, task_id, "setup_ap_connected")

        configured = _portal_json("/api/pairing", allow_404=True)
        if configured is not None:
            resumed = True
        else:
            networks = _portal_json("/api/networks", timeout=args.portal_timeout)
            assert networks is not None
            display_name = args.display_name or networks.get("suggested_name")
            hostname = args.hostname or networks.get("suggested_hostname")
            if not isinstance(display_name, str) or not display_name:
                raise UserError("device did not provide a suggested display name")
            if not isinstance(hostname, str) or not HOSTNAME_RE.fullmatch(hostname):
                raise UserError("device did not provide a valid suggested hostname")
            configured = _portal_json(
                "/api/configure",
                "POST",
                {
                    "display_name": display_name,
                    "hostname": hostname,
                    "ssid": home_ssid,
                    "password": home_password,
                },
                timeout=args.configure_timeout,
            )
            assert configured is not None

        connection = _connection_from_portal(configured)
        update_pairing_task(
            store,
            task_id,
            "credential_received",
            device_uid=connection["device_uid"],
        )
        save_connection_record(
            connection,
            store,
            args.replace,
            pairing_state="device_pending",
        )
        connection = reload_connection_record(store, connection)
        update_pairing_task(
            store,
            task_id,
            "credential_stored",
            device_uid=connection["device_uid"],
            credential_state="stored",
        )
        _portal_json("/api/complete", "POST", {}, timeout=args.portal_timeout)
        paired = True
        registry = load_registry(store)
        _, record = select_record(registry, connection["device_uid"])
        record["pairing"] = {
            "state": "completed",
            "completed_at": utc_now(),
            "confirmed_by": "setup_portal",
        }
        registry["updated_at"] = utc_now()
        save_registry(store, registry)
        update_pairing_task(
            store,
            task_id,
            "device_pairing_completed",
            device_uid=connection["device_uid"],
            credential_state="stored",
        )
    except BaseException as exc:  # Preserve cleanup for Ctrl-C and unexpected failures.
        workflow_error = exc
    finally:
        home_password = ""
        if switched:
            try:
                _run_nmcli(
                    ["connection", "up", home_profile, "ifname", interface], 45.0
                )
            except UserError as exc:
                restore_error = exc
        try:
            _run_nmcli(["connection", "delete", temporary_profile])
        except UserError:
            pass
        for stale_profile, stale_interface in stale_temporary_profiles:
            if stale_profile == temporary_profile or stale_interface != interface:
                continue
            try:
                _run_nmcli(["connection", "delete", stale_profile])
            except UserError:
                pass

    if workflow_error:
        state, reason = _failure_task_state(workflow_error, connection)
        if restore_error:
            state = "attention_required"
            reason = f"{reason}; household_wifi_restore_failed"
        update_pairing_task(
            store,
            task_id,
            state,
            reason=reason,
            household_wifi_restored=restore_error is None,
        )
        if restore_error:
            raise UserError(
                f"pairing failed and the household Wi-Fi could not be restored: {restore_error}"
            ) from workflow_error
        raise workflow_error
    if restore_error:
        update_pairing_task(
            store,
            task_id,
            "attention_required",
            reason="household_wifi_restore_failed",
            household_wifi_restored=False,
        )
        raise UserError(
            f"device workflow finished but the household Wi-Fi could not be restored: {restore_error}"
        )
    assert connection is not None

    verification_state = "unverified"
    verification_warning = None
    if not args.skip_verify:
        time.sleep(args.settle_seconds)
        try:
            verification, _ = verify_connection(
                connection, connection["device_uid"], args.verify_timeout
            )
            registry = load_registry(store)
            _, record = select_record(registry, connection["device_uid"])
            record["verification"] = verification
            record["pairing"] = {
                "state": "completed",
                "confirmed_by": "authenticated_manifest",
            }
            record.setdefault("plugin", {"state": "disabled"})
            record.setdefault("grant", {"state": "not_granted"})
            registry["updated_at"] = utc_now()
            save_registry(store, registry)
            verification_state = "verified"
        except UserError as exc:
            verification_warning = str(exc)

    update_pairing_task(
        store,
        task_id,
        "completed",
        device_uid=connection["device_uid"],
        credential_state="stored",
        verification_state=verification_state,
        household_wifi_restored=True,
        completed_at=utc_now(),
    )

    output = {
        "ok": True,
        "headless": True,
        "paired": paired,
        "pairing_task_id": task_id,
        "pairing_task_resumed": task_resumed,
        "resumed_pending_pairing": resumed,
        "device_uid": connection["device_uid"],
        "name": connection["name"],
        "url": connection["url"],
        "contract": _connection_contract(connection)[0],
        "credential_store": str(store),
        "credential": "stored",
        "verification": verification_state,
        "plugin": "disabled",
        "grant": "not_granted",
        "capability_enabled": False,
    }
    if verification_warning:
        output["verification_warning"] = verification_warning
    print(json.dumps(output, ensure_ascii=False, indent=2))


def _avahi_unescape(value: str) -> str:
    value = value.strip('"')
    output = bytearray()
    index = 0
    while index < len(value):
        if value[index] == "\\" and value[index + 1 : index + 4].isdigit():
            output.append(int(value[index + 1 : index + 4], 10))
            index += 4
        else:
            output.extend(value[index].encode("utf-8"))
            index += 1
    return output.decode("utf-8", errors="replace")


def _avahi_txt_fields(fields: list[str]) -> list[str]:
    """Split Avahi's quoted TXT block while accepting older semicolon fixtures."""
    raw = " ".join(fields)
    try:
        return shlex.split(raw)
    except ValueError:
        return fields


def discover_devices(args: argparse.Namespace) -> None:
    executable = shutil.which("avahi-browse")
    if not executable:
        raise UserError(
            "avahi-browse is not installed; install Avahi tools with user approval"
        )
    discovered: list[dict[str, Any]] = []
    seen: set[tuple[str, str, str, int]] = set()
    for discovery_service, browse_service in DISCOVERY_SERVICES.items():
        try:
            result = subprocess.run(
                [executable, "-rtpk", browse_service],
                capture_output=True,
                text=True,
                timeout=args.timeout,
                check=False,
            )
        except subprocess.TimeoutExpired as exc:
            raise UserError(f"mDNS discovery timed out for {browse_service}") from exc
        if result.returncode != 0:
            detail = result.stderr.strip().splitlines()
            message = detail[-1] if detail else "mDNS discovery failed"
            raise UserError(f"{browse_service}: {message}")
        for line in result.stdout.splitlines():
            if not line.startswith("="):
                continue
            parts = line.split(";")
            if len(parts) < 9:
                continue
            try:
                port = int(parts[8])
            except ValueError:
                continue
            txt: dict[str, str] = {}
            for field in _avahi_txt_fields(parts[9:]):
                decoded = _avahi_unescape(field)
                if "=" in decoded:
                    key, value = decoded.split("=", 1)
                    txt[key] = value
            uid = txt.get("device_uid", "")
            key = (discovery_service, uid, parts[6], port)
            if key in seen:
                continue
            seen.add(key)
            discovered.append(
                {
                    "discovery": discovery_service,
                    "device_uid": uid or None,
                    "display_name": txt.get("display_name")
                    or _avahi_unescape(parts[3]),
                    "hostname": parts[6],
                    "address": parts[7],
                    "port": port,
                    "path": txt.get("path", "/mcp"),
                    "transport": txt.get("transport"),
                    "device_type": txt.get("device_type"),
                    "cmpp_version": txt.get("cmpp_version"),
                    "mcp_version": txt.get("mcp_version"),
                    "hap_version": txt.get("hap_version"),
                    "auth": txt.get("auth"),
                    "firmware": txt.get("firmware"),
                    "pairing": txt.get("pairing"),
                    "trusted": False,
                }
            )
    print(
        json.dumps(
            {
                "services": sorted(DISCOVERY_SERVICES),
                "warning": "mDNS records are discovery hints and are not authenticated",
                "devices": discovered,
            },
            ensure_ascii=False,
            indent=2,
        )
    )


def remove_device(args: argparse.Namespace) -> None:
    if not args.yes:
        raise UserError("remove requires explicit --yes confirmation")
    store = resolve_store(args.store)
    registry = load_registry(store)
    uid, record = select_record(registry, args.selector)
    name = record.get("connection", {}).get("name")
    del registry["devices"][uid]
    registry["updated_at"] = utc_now()
    save_registry(store, registry)
    print(
        json.dumps(
            {"ok": True, "removed_device_uid": uid, "name": name},
            ensure_ascii=False,
            indent=2,
        )
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Pair and securely register official Caelus MCP devices"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    store_parent = argparse.ArgumentParser(add_help=False)
    store_parent.add_argument(
        "--store",
        help="credential registry path (default: ~/.config/caelus/mcp-devices.json)",
    )

    import_parser = subparsers.add_parser(
        "import", parents=[store_parent], help="validate and save a pairing JSON file"
    )
    import_parser.add_argument("file", help="downloaded .mcp.json file, or - for stdin")
    import_parser.add_argument(
        "--replace",
        action="store_true",
        help="replace a different Token for the same device UID",
    )
    import_parser.set_defaults(handler=import_connection)

    provision_parser = subparsers.add_parser(
        "provision",
        parents=[store_parent],
        help="perform unattended first-time provisioning through NetworkManager",
    )
    provision_parser.add_argument(
        "--device-ap",
        help="exact Caelus-* setup SSID; required when multiple are visible",
    )
    provision_parser.add_argument(
        "--wifi-connection",
        help="saved NetworkManager credential-source profile (default: active Wi-Fi)",
    )
    provision_parser.add_argument(
        "--wifi-ssid",
        help="target 2.4 GHz SSID when reusing the selected profile's saved PSK",
    )
    provision_parser.add_argument(
        "--interface", help="NetworkManager Wi-Fi interface (default: active interface)"
    )
    provision_parser.add_argument("--display-name", help="device display name")
    provision_parser.add_argument(
        "--hostname", help="device .local hostname without .local"
    )
    provision_parser.add_argument(
        "--replace",
        action="store_true",
        help="replace a different Token for the same device UID",
    )
    provision_parser.add_argument(
        "--yes",
        action="store_true",
        help="authorize Wi-Fi switching and transmission of the saved network PSK",
    )
    provision_parser.add_argument(
        "--dry-run",
        action="store_true",
        help="inspect prerequisites and selection without changing the network",
    )
    provision_parser.add_argument(
        "--skip-verify",
        action="store_true",
        help="save the pairing but skip post-provisioning MCP verification",
    )
    provision_parser.add_argument("--portal-timeout", type=float, default=20.0)
    provision_parser.add_argument("--configure-timeout", type=float, default=45.0)
    provision_parser.add_argument("--verify-timeout", type=float, default=15.0)
    provision_parser.add_argument("--settle-seconds", type=float, default=5.0)
    provision_parser.set_defaults(handler=provision_device)

    list_parser = subparsers.add_parser(
        "list",
        parents=[store_parent],
        help="list saved devices without revealing Tokens",
    )
    list_parser.set_defaults(handler=list_devices)

    show_parser = subparsers.add_parser(
        "show", parents=[store_parent], help="show a saved device with a masked Token"
    )
    show_parser.add_argument("selector", help="device UID, name, or hostname")
    show_parser.add_argument(
        "--reveal-token", action="store_true", help="print the full Token (sensitive)"
    )
    show_parser.set_defaults(handler=show_device)

    tasks_parser = subparsers.add_parser(
        "tasks",
        parents=[store_parent],
        help="list durable pairing tasks without revealing credentials",
    )
    tasks_parser.set_defaults(handler=list_pairing_tasks)

    verify_parser = subparsers.add_parser(
        "verify",
        parents=[store_parent],
        help="perform a read-only MCP and manifest verification",
    )
    verify_parser.add_argument("selector", help="device UID, name, or hostname")
    verify_parser.add_argument("--timeout", type=float, default=10.0)
    verify_parser.set_defaults(handler=verify_device)

    discover_parser = subparsers.add_parser(
        "discover", help="list unauthenticated CMPP and legacy Caelus mDNS hints"
    )
    discover_parser.add_argument("--timeout", type=float, default=10.0)
    discover_parser.set_defaults(handler=discover_devices)

    remove_parser = subparsers.add_parser(
        "remove", parents=[store_parent], help="remove a saved credential"
    )
    remove_parser.add_argument("selector", help="device UID, name, or hostname")
    remove_parser.add_argument(
        "--yes", action="store_true", help="confirm credential removal"
    )
    remove_parser.set_defaults(handler=remove_device)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if hasattr(args, "timeout") and not 0.5 <= args.timeout <= 60:
        parser.error("--timeout must be between 0.5 and 60 seconds")
    timeout_limits = {
        "portal_timeout": (3.0, 120.0),
        "configure_timeout": (10.0, 120.0),
        "verify_timeout": (1.0, 60.0),
        "settle_seconds": (0.0, 60.0),
    }
    for field, (minimum, maximum) in timeout_limits.items():
        if hasattr(args, field) and not minimum <= getattr(args, field) <= maximum:
            option = field.replace("_", "-")
            parser.error(
                f"--{option} must be between {minimum:g} and {maximum:g} seconds"
            )
    try:
        args.handler(args)
    except UserError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
