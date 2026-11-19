#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name("caelus_devices.py")
SPEC = importlib.util.spec_from_file_location("caelus_devices", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def connection(
    token: str = "a" * 64, discovery: str = "_cmpp._tcp.local"
) -> dict:
    return {
        "schema": "caelus.mcp.connection.v1",
        "name": "Caelus Light 1",
        "device_uid": "hw_0123456789abcdef",
        "hostname": "caelus-light-1",
        "transport": "streamable_http",
        "url": "http://caelus-light-1.local:8080/mcp",
        "headers": {"Authorization": f"Bearer {token}"},
        "discovery": discovery,
    }


def portal_response(token: str = "a" * 64) -> dict:
    saved = connection(token)
    return {
        "ok": True,
        "device": {
            "display_name": saved["name"],
            "device_uid": saved["device_uid"],
            "hostname": saved["hostname"],
            "discovery": saved["discovery"],
        },
        "connection": {
            "transport": saved["transport"],
            "url": saved["url"],
            "headers": saved["headers"],
        },
    }


def native_manifest() -> dict:
    return {
        "schema": "cmpp.manifest.v1",
        "protocol_version": "1.0",
        "device_id": "hw_0123456789abcdef",
        "profile_id": "caelus.test-board.v1",
        "state_version": 0,
        "capabilities": [
            {
                "capability_id": "test_board.pwm",
                "display_name": "Test board indicator LEDs",
                "version": "1.0",
                "command_types": ["test_board.pwm.set"],
                "allowed_safety_mask": 1,
                "requires_ack": False,
                "supports_dry_run": True,
            },
            {
                "capability_id": "test_board.relay",
                "display_name": "Test board relays",
                "version": "1.0",
                "command_types": [
                    "test_board.relay.set",
                    "test_board.relay.pulse",
                ],
                "allowed_safety_mask": 7,
                "requires_ack": True,
                "supports_dry_run": True,
            },
        ],
    }


def legacy_manifest() -> dict:
    legacy_connection = connection(discovery="_caelus-hw._tcp.local")
    return {
        "schema": "caelus.hardware.manifest.v0",
        "protocol_version": "0.1.0",
        "vendor": "Caelus",
        "device_uid": "hw_0123456789abcdef",
        "pairing": {"state": "paired", "method": "bearer"},
        "transports": [
            {
                "type": "streamable_http",
                "endpoint": legacy_connection["url"],
                "encrypted": False,
            }
        ],
        "capabilities": [],
        "commands": [],
        "cloud_egress": {"required": False, "declared_hosts": []},
        "privacy": {"raw_media_default": "not_emitted"},
        "x_cmpp_compatibility": {
            "surface": "legacy_mcp",
            "mcp_version": "2025-11-25",
            "legacy_connection_schema": "caelus.mcp.connection.v1",
            "native_target": "caelus.hardware.profile.v0",
            "connector_required": True,
        },
    }


class CaelusDevicesTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.store_dir = self.root / "private"
        self.store_dir.mkdir(mode=0o700)
        self.store = self.store_dir / "devices.json"
        self.input = self.root / "device.mcp.json"
        self.input.write_text(json.dumps(connection()), encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), *arguments],
            text=True,
            capture_output=True,
            check=False,
        )

    def provision_args(self, **overrides) -> SimpleNamespace:
        values = {
            "yes": True,
            "dry_run": False,
            "interface": None,
            "wifi_connection": None,
            "wifi_ssid": None,
            "device_ap": None,
            "store": str(self.store),
            "display_name": None,
            "hostname": None,
            "replace": False,
            "skip_verify": True,
            "portal_timeout": 20.0,
            "configure_timeout": 45.0,
            "verify_timeout": 15.0,
            "settle_seconds": 0.0,
        }
        values.update(overrides)
        return SimpleNamespace(**values)

    def test_import_is_atomic_private_and_output_is_sanitized(self) -> None:
        result = self.run_cli("import", str(self.input), "--store", str(self.store))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("a" * 64, result.stdout)
        self.assertEqual(stat.S_IMODE(self.store.stat().st_mode), 0o600)
        saved = json.loads(self.store.read_text(encoding="utf-8"))
        record = saved["devices"]["hw_0123456789abcdef"]
        self.assertEqual(record["credential"]["state"], "stored")
        self.assertEqual(record["contract"], "native_cmpp_v1")
        self.assertEqual(record["verification"]["state"], "unverified")
        self.assertEqual(record["plugin"]["state"], "disabled")
        self.assertEqual(record["grant"]["state"], "not_granted")
        self.assertEqual(
            record["connection"]["headers"]["Authorization"],
            f"Bearer {'a' * 64}",
        )

    def test_list_and_show_mask_token(self) -> None:
        self.assertEqual(
            self.run_cli(
                "import", str(self.input), "--store", str(self.store)
            ).returncode,
            0,
        )
        listed = self.run_cli("list", "--store", str(self.store))
        shown = self.run_cli("show", "Caelus Light 1", "--store", str(self.store))
        self.assertEqual(listed.returncode, 0, listed.stderr)
        self.assertEqual(shown.returncode, 0, shown.stderr)
        self.assertNotIn("a" * 64, listed.stdout)
        self.assertNotIn("a" * 64, shown.stdout)
        self.assertIn("******", shown.stdout)

    def test_different_token_requires_replace(self) -> None:
        self.assertEqual(
            self.run_cli(
                "import", str(self.input), "--store", str(self.store)
            ).returncode,
            0,
        )
        other_token = "b" * 64
        self.input.write_text(json.dumps(connection(other_token)), encoding="utf-8")
        result = self.run_cli("import", str(self.input), "--store", str(self.store))
        self.assertEqual(result.returncode, 2)
        self.assertIn("different Token", result.stderr)
        self.assertNotIn(other_token, result.stderr)

    def test_rejects_public_endpoint_and_wifi_secret(self) -> None:
        public = connection()
        public["url"] = "https://example.com/mcp"
        with self.assertRaises(MODULE.UserError):
            MODULE.validate_connection(public)
        leaked = connection()
        leaked["ssid"] = "private-network"
        with self.assertRaises(MODULE.UserError):
            MODULE.validate_connection(leaked)

    def test_accepts_native_and_legacy_discovery_only(self) -> None:
        MODULE.validate_connection(connection())
        MODULE.validate_connection(connection(discovery="_caelus-hw._tcp.local"))
        unsupported = connection()
        unsupported["discovery"] = "_other._tcp.local"
        with self.assertRaisesRegex(MODULE.UserError, "discovery must be one of"):
            MODULE.validate_connection(unsupported)

    def test_parses_json_and_sse_mcp_responses(self) -> None:
        expected = {"jsonrpc": "2.0", "id": 1, "result": {}}
        self.assertEqual(
            MODULE._parse_mcp_body(json.dumps(expected).encode()), expected
        )
        sse = f"event: message\ndata: {json.dumps(expected)}\n\n".encode()
        self.assertEqual(MODULE._parse_mcp_body(sse), expected)

    def test_native_manifest_requires_current_test_board_profile(self) -> None:
        details = MODULE.validate_manifest(
            native_manifest(), "hw_0123456789abcdef", connection()
        )
        self.assertEqual(details["contract"], "native_cmpp_v1")
        self.assertEqual(details["profile_id"], "caelus.test-board.v1")
        self.assertFalse(details["official_device"])
        self.assertEqual(details["vendor_attestation"], "unavailable")

        missing_relay = native_manifest()
        missing_relay["capabilities"].pop()
        with self.assertRaisesRegex(MODULE.UserError, "missing capabilities"):
            MODULE.validate_manifest(
                missing_relay, "hw_0123456789abcdef", connection()
            )

        wrong_uid = native_manifest()
        wrong_uid["device_id"] = "hw_deadbeefdeadbeef"
        with self.assertRaisesRegex(MODULE.UserError, "does not match"):
            MODULE.validate_manifest(
                wrong_uid, "hw_0123456789abcdef", connection()
            )

    def test_legacy_manifest_remains_supported(self) -> None:
        legacy_connection = connection(discovery="_caelus-hw._tcp.local")
        manifest = legacy_manifest()
        details = MODULE.validate_manifest(
            manifest, "hw_0123456789abcdef", legacy_connection
        )
        self.assertTrue(details["official_device"])
        manifest["vendor"] = "Untrusted"
        with self.assertRaises(MODULE.UserError):
            MODULE.validate_manifest(
                manifest, "hw_0123456789abcdef", legacy_connection
            )

    def test_native_verify_is_read_only_and_matches_current_contract(self) -> None:
        manifest = native_manifest()

        def mcp_post(_connection, payload, _timeout, session=None):
            method = payload["method"]
            if method == "initialize":
                return (
                    {
                        "jsonrpc": "2.0",
                        "id": 1,
                        "result": {
                            "protocolVersion": "2025-11-25",
                            "serverInfo": {"name": "caelus-cmpp-device"},
                        },
                    },
                    "session-1",
                )
            self.assertEqual(session, "session-1")
            if method == "notifications/initialized":
                return None, session
            if method == "tools/list":
                return (
                    {
                        "jsonrpc": "2.0",
                        "id": 2,
                        "result": {"tools": [{"name": "cmpp.execute"}]},
                    },
                    session,
                )
            if method == "resources/list":
                return (
                    {
                        "jsonrpc": "2.0",
                        "id": 3,
                        "result": {
                            "resources": [
                                {"uri": "cmpp://manifest"},
                                {"uri": "cmpp://state"},
                            ]
                        },
                    },
                    session,
                )
            if method == "resources/read":
                self.assertEqual(payload["params"]["uri"], "cmpp://manifest")
                return (
                    {
                        "jsonrpc": "2.0",
                        "id": 4,
                        "result": {
                            "contents": [
                                {
                                    "uri": "cmpp://manifest",
                                    "mimeType": "application/json",
                                    "text": json.dumps(manifest),
                                }
                            ]
                        },
                    },
                    session,
                )
            self.fail(f"unexpected MCP method {method}")

        with mock.patch.object(MODULE, "_mcp_post", side_effect=mcp_post) as post:
            verification, summary = MODULE.verify_connection(
                connection(), "hw_0123456789abcdef", 5.0
            )

        self.assertEqual(post.call_count, 5)
        self.assertEqual(verification["contract"], "native_cmpp_v1")
        self.assertEqual(verification["manifest_uri"], "cmpp://manifest")
        self.assertEqual(summary["profile_id"], "caelus.test-board.v1")
        self.assertEqual(
            summary["capabilities"], ["test_board.pwm", "test_board.relay"]
        )
        self.assertFalse(summary["official_device"])
        self.assertEqual(summary["vendor_attestation"], "unavailable")
        self.assertFalse(summary["capability_enabled"])

    def test_discovery_browses_native_and_legacy_services(self) -> None:
        native_line = (
            '=;wlp2s0;IPv4;Caelus Board;_cmpp._tcp;local;'
            'caelus-light-1.local;192.168.1.20;8080;"path=/mcp" '
            '"transport=streamable-http" "device_type=cmpp-test-board" '
            '"cmpp_version=1.0" "mcp_version=2025-11-25" '
            '"device_uid=hw_0123456789abcdef" "pairing=paired" '
            '"auth=local_token" "firmware=1.0.0"\n'
        )
        runs = [
            subprocess.CompletedProcess([], 0, native_line, ""),
            subprocess.CompletedProcess([], 0, "", ""),
        ]
        output = io.StringIO()
        with (
            mock.patch.object(MODULE.shutil, "which", return_value="/usr/bin/avahi-browse"),
            mock.patch.object(MODULE.subprocess, "run", side_effect=runs) as run,
            contextlib.redirect_stdout(output),
        ):
            MODULE.discover_devices(SimpleNamespace(timeout=5.0))

        calls = [call.args[0][-1] for call in run.call_args_list]
        self.assertEqual(calls, ["_cmpp._tcp", "_caelus-hw._tcp"])
        result = json.loads(output.getvalue())
        self.assertEqual(
            result["services"], ["_caelus-hw._tcp.local", "_cmpp._tcp.local"]
        )
        self.assertEqual(result["devices"][0]["discovery"], "_cmpp._tcp.local")
        self.assertEqual(result["devices"][0]["cmpp_version"], "1.0")
        self.assertFalse(result["devices"][0]["trusted"])

    def test_credential_readback_and_pairing_task_resume(self) -> None:
        MODULE.save_connection_record(
            connection(), self.store, pairing_state="device_pending"
        )
        self.assertEqual(
            MODULE.reload_connection_record(self.store, connection()), connection()
        )

        task_id, resumed = MODULE.start_pairing_task(
            self.store,
            device_ap="Caelus-Light-A1B2C3",
            interface="wlp2s0",
            household_connection="Home",
            household_ssid="Home WiFi",
        )
        self.assertFalse(resumed)
        MODULE.update_pairing_task(self.store, task_id, "interrupted")
        resumed_id, resumed = MODULE.start_pairing_task(
            self.store,
            device_ap="Caelus-Light-A1B2C3",
            interface="wlp2s0",
            household_connection="Home",
            household_ssid="Home WiFi",
        )
        self.assertTrue(resumed)
        self.assertEqual(resumed_id, task_id)
        saved = json.loads(self.store.read_text(encoding="utf-8"))
        task = saved["pairing_tasks"][task_id]
        self.assertEqual(task["attempts"], 2)
        self.assertNotIn("password", json.dumps(task).casefold())
        tasks = self.run_cli("tasks", "--store", str(self.store))
        self.assertEqual(tasks.returncode, 0, tasks.stderr)
        self.assertIn(task_id, tasks.stdout)
        self.assertNotIn("a" * 64, tasks.stdout)

    def test_recovers_household_profile_after_interrupted_wifi_switch(self) -> None:
        task_id, _ = MODULE.start_pairing_task(
            self.store,
            device_ap="Caelus-Light-A1B2C3",
            interface="wlp2s0",
            household_connection="Home",
            household_ssid="Home WiFi",
        )
        MODULE.update_pairing_task(self.store, task_id, "network_switching")
        output = io.StringIO()
        with (
            mock.patch.object(
                MODULE,
                "_active_wifi",
                return_value=[("caelus-provision-stale", "wlp2s0")],
            ),
            mock.patch.object(
                MODULE, "_profile_credentials", return_value=("Home WiFi", "secret123")
            ) as credentials,
            mock.patch.object(
                MODULE, "_select_setup_ap", return_value="Caelus-Light-A1B2C3"
            ),
            contextlib.redirect_stdout(output),
        ):
            MODULE.provision_device(self.provision_args(dry_run=True, yes=False))

        credentials.assert_called_once_with("Home")
        result = json.loads(output.getvalue())
        self.assertTrue(result["recovered_interrupted_task"])
        self.assertEqual(
            result["stale_temporary_profiles"], ["caelus-provision-stale"]
        )

    def test_nmcli_escaped_fields_and_portal_connection(self) -> None:
        self.assertEqual(
            MODULE._nmcli_fields(r"Home\:WiFi:802-11-wireless:wlp2s0"),
            ["Home:WiFi", "802-11-wireless", "wlp2s0"],
        )
        self.assertEqual(
            MODULE._connection_from_portal(portal_response()), connection()
        )
        with self.assertRaises(MODULE.UserError):
            MODULE._connection_from_portal({"ok": True})

    def test_unattended_provision_saves_before_complete_and_restores_wifi(self) -> None:
        nmcli_calls = []
        portal_calls = []

        def run_nmcli(arguments, timeout=30.0):
            nmcli_calls.append((arguments, timeout))
            return ""

        def call_portal(path, method="GET", payload=None, **_kwargs):
            portal_calls.append(path)
            if path == "/api/pairing":
                return None
            if path == "/api/networks":
                return {
                    "ok": True,
                    "suggested_name": "Caelus Light 1",
                    "suggested_hostname": "caelus-light-1",
                }
            if path == "/api/configure":
                self.assertEqual(method, "POST")
                self.assertEqual(payload["ssid"], "Home WiFi")
                self.assertEqual(payload["password"], "not-in-output")
                return portal_response()
            if path == "/api/complete":
                self.assertTrue(self.store.exists(), "credential must be durable first")
                saved = json.loads(self.store.read_text(encoding="utf-8"))
                record = saved["devices"]["hw_0123456789abcdef"]
                self.assertEqual(record["credential"]["state"], "stored")
                task = next(iter(saved["pairing_tasks"].values()))
                self.assertEqual(task["state"], "credential_stored")
                return {"ok": True}
            self.fail(f"unexpected portal path {path}")

        output = io.StringIO()
        with (
            mock.patch.object(
                MODULE, "_active_wifi", return_value=[("Home", "wlp2s0")]
            ),
            mock.patch.object(
                MODULE,
                "_profile_credentials",
                return_value=("Home WiFi 5G", "not-in-output"),
            ),
            mock.patch.object(
                MODULE, "_select_setup_ap", return_value="Caelus-Light-A1B2C3"
            ),
            mock.patch.object(MODULE, "_run_nmcli", side_effect=run_nmcli),
            mock.patch.object(MODULE, "_wait_for_portal"),
            mock.patch.object(MODULE, "_portal_json", side_effect=call_portal),
            contextlib.redirect_stdout(output),
        ):
            MODULE.provision_device(self.provision_args(wifi_ssid="Home WiFi"))

        result = json.loads(output.getvalue())
        self.assertTrue(result["headless"])
        self.assertTrue(result["paired"])
        self.assertEqual(result["credential"], "stored")
        self.assertEqual(result["plugin"], "disabled")
        self.assertEqual(result["grant"], "not_granted")
        self.assertNotIn("not-in-output", output.getvalue())
        self.assertEqual(
            portal_calls,
            ["/api/pairing", "/api/networks", "/api/configure", "/api/complete"],
        )
        commands = [call[0] for call in nmcli_calls]
        self.assertTrue(
            any(command[:3] == ["connection", "up", "Home"] for command in commands)
        )
        self.assertTrue(
            any(command[:2] == ["connection", "delete"] for command in commands)
        )
        saved = json.loads(self.store.read_text(encoding="utf-8"))
        task = saved["pairing_tasks"][result["pairing_task_id"]]
        self.assertEqual(task["state"], "completed")
        self.assertTrue(task["household_wifi_restored"])

    def test_unattended_failure_restores_wifi_without_completing(self) -> None:
        nmcli_calls = []
        portal_calls = []

        def run_nmcli(arguments, timeout=30.0):
            nmcli_calls.append((arguments, timeout))
            return ""

        def call_portal(path, *_args, **_kwargs):
            portal_calls.append(path)
            if path == "/api/pairing":
                return None
            if path == "/api/networks":
                return {
                    "ok": True,
                    "suggested_name": "Caelus Light 1",
                    "suggested_hostname": "caelus-light-1",
                }
            raise MODULE.UserError("simulated setup failure")

        with (
            mock.patch.object(
                MODULE, "_active_wifi", return_value=[("Home", "wlp2s0")]
            ),
            mock.patch.object(
                MODULE, "_profile_credentials", return_value=("Home WiFi", "secret123")
            ),
            mock.patch.object(
                MODULE, "_select_setup_ap", return_value="Caelus-Light-A1B2C3"
            ),
            mock.patch.object(MODULE, "_run_nmcli", side_effect=run_nmcli),
            mock.patch.object(MODULE, "_wait_for_portal"),
            mock.patch.object(MODULE, "_portal_json", side_effect=call_portal),
        ):
            with self.assertRaisesRegex(MODULE.UserError, "simulated setup failure"):
                MODULE.provision_device(self.provision_args())

        self.assertNotIn("/api/complete", portal_calls)
        commands = [call[0] for call in nmcli_calls]
        self.assertTrue(
            any(command[:3] == ["connection", "up", "Home"] for command in commands)
        )
        self.assertTrue(
            any(command[:2] == ["connection", "delete"] for command in commands)
        )
        saved = json.loads(self.store.read_text(encoding="utf-8"))
        task = next(iter(saved["pairing_tasks"].values()))
        self.assertEqual(task["state"], "failed")
        self.assertTrue(task["household_wifi_restored"])

    def test_unattended_provision_resumes_pending_pairing(self) -> None:
        portal_calls = []

        def call_portal(path, *_args, **_kwargs):
            portal_calls.append(path)
            if path == "/api/pairing":
                return portal_response()
            if path == "/api/complete":
                return {"ok": True}
            self.fail(f"unexpected portal path {path}")

        output = io.StringIO()
        with (
            mock.patch.object(
                MODULE, "_active_wifi", return_value=[("Home", "wlp2s0")]
            ),
            mock.patch.object(
                MODULE, "_profile_credentials", return_value=("Home WiFi", "secret123")
            ),
            mock.patch.object(
                MODULE, "_select_setup_ap", return_value="Caelus-Light-A1B2C3"
            ),
            mock.patch.object(MODULE, "_run_nmcli", return_value=""),
            mock.patch.object(MODULE, "_wait_for_portal"),
            mock.patch.object(MODULE, "_portal_json", side_effect=call_portal),
            contextlib.redirect_stdout(output),
        ):
            MODULE.provision_device(self.provision_args())

        self.assertTrue(json.loads(output.getvalue())["resumed_pending_pairing"])
        self.assertEqual(portal_calls, ["/api/pairing", "/api/complete"])


if __name__ == "__main__":
    unittest.main()
