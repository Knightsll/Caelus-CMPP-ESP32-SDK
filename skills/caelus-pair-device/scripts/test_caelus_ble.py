#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name("caelus_ble.py")
SPEC = importlib.util.spec_from_file_location("caelus_ble", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class IdentitySecurity:
    verbose = False

    def security_session(self, response):
        return None

    def encrypt_data(self, value):
        return value

    def decrypt_data(self, value):
        return value


class CaelusBleTest(unittest.IsolatedAsyncioTestCase):
    def test_select_device_requires_an_unambiguous_target(self) -> None:
        one = [{"name": "Caelus-Board-A1B2C3"}]
        self.assertEqual(MODULE._select_device(one, None), one[0])
        with self.assertRaises(MODULE.registry.UserError):
            MODULE._select_device([], None)
        with self.assertRaises(MODULE.registry.UserError):
            MODULE._select_device(one + [{"name": "Caelus-Board-D4E5F6"}], None)

    def test_connection_is_assembled_without_exposing_wifi_secret(self) -> None:
        saved = MODULE._connection_from_ble(
            {"device_uid": "hw_0123456789abcdef", "auth_token": "a" * 64},
            "Desk lamp",
            "desk-lamp",
        )
        self.assertEqual(saved["url"], "http://desk-lamp.local:8080/mcp")
        self.assertNotIn("ssid", json.dumps(saved).casefold())
        self.assertNotIn("password", json.dumps(saved).casefold())

    async def test_ble_provision_stores_credential_before_complete(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        store = Path(temporary.name) / "private" / "devices.json"
        calls: list[str] = []

        class FakeTransport:
            def __init__(self, **_kwargs):
                pass

            async def connect(self, devname):
                calls.append(f"connect:{devname}")

            async def disconnect(self):
                calls.append("disconnect")

            async def send_data(self, endpoint, payload):
                self.assert_endpoint(endpoint)
                value = json.loads(payload)
                calls.append(value["action"])
                if value["action"] == "pair":
                    return json.dumps(
                        {
                            "ok": True,
                            "device_uid": "hw_0123456789abcdef",
                            "auth_token": "a" * 64,
                        }
                    )
                self_outer.assertTrue(store.exists())
                saved = json.loads(store.read_text(encoding="utf-8"))
                self_outer.assertIn(
                    "hw_0123456789abcdef", saved["devices"]
                )
                return '{"ok":true}'

            @staticmethod
            def assert_endpoint(endpoint):
                if endpoint != "custom-data":
                    raise AssertionError(endpoint)

        self_outer = self
        fake_transport_module = SimpleNamespace(Transport_BLE=FakeTransport)
        fake_security_module = SimpleNamespace(
            Security1=lambda _pop, _verbose: IdentitySecurity()
        )
        args = SimpleNamespace(
            yes=True,
            dry_run=False,
            wifi_connection=None,
            wifi_ssid=None,
            device=None,
            store=str(store),
            display_name=None,
            hostname=None,
            replace=False,
            skip_verify=True,
            scan_timeout=1.0,
            configure_timeout=10.0,
            verify_timeout=1.0,
            settle_seconds=0.0,
            adapter="hci0",
        )

        output = io.StringIO()
        with (
            mock.patch.object(
                MODULE.registry, "_active_wifi", return_value=[("Home", "wlan0")]
            ),
            mock.patch.object(
                MODULE.registry,
                "_profile_credentials",
                return_value=("Home WiFi", "not-in-output"),
            ),
            mock.patch.object(
                MODULE,
                "scan_devices",
                return_value=[
                    {
                        "name": "Caelus-Board-A1B2C3",
                        "address": "00:11:22:33:44:55",
                        "rssi": -45,
                        "service_uuid": MODULE.BLE_SERVICE_UUID,
                    }
                ],
            ),
            mock.patch.object(
                MODULE,
                "_load_ble_stack",
                return_value=(None, SimpleNamespace(), fake_security_module, fake_transport_module),
            ),
            mock.patch.object(
                MODULE,
                "_capabilities",
                return_value={"caelus_pairing_v1"},
            ),
            mock.patch.object(MODULE, "_establish_session"),
            contextlib.redirect_stdout(output),
        ):
            await MODULE.provision(args)

        self.assertEqual(
            calls,
            ["connect:Caelus-Board-A1B2C3", "pair", "complete", "disconnect"],
        )
        self.assertNotIn("not-in-output", output.getvalue())
        result = json.loads(output.getvalue())
        self.assertEqual(result["credential"], "stored")
        self.assertEqual(result["device_pairing"], "completed")
        saved = json.loads(store.read_text(encoding="utf-8"))
        self.assertEqual(
            saved["devices"]["hw_0123456789abcdef"]["pairing"]["state"],
            "completed",
        )


if __name__ == "__main__":
    unittest.main()
