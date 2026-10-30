#!/usr/bin/env python3
"""Create the isolated Python environment required by Caelus BLE pairing."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import venv


def environment_root() -> Path:
    configured = os.environ.get("CAELUS_BLE_VENV")
    if configured:
        return Path(configured).expanduser()
    return Path.home() / ".cache" / "caelus-pair-device" / "venv"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Install BLE client dependencies into an isolated cache venv"
    )
    parser.add_argument(
        "--yes",
        action="store_true",
        help="authorize creation/update of the cache venv and package downloads",
    )
    args = parser.parse_args()
    if not args.yes:
        parser.error("rerun with --yes to authorize package downloads")

    root = environment_root()
    root.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    venv.EnvBuilder(
        with_pip=True, clear=False, system_site_packages=True
    ).create(root)
    python = root / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    result = subprocess.run(
        [
            str(python),
            "-m",
            "pip",
            "install",
            "bleak>=0.22,<2",
            "cryptography>=42,<47",
            "protobuf>=5,<7",
        ],
        check=False,
    )
    if result.returncode != 0:
        print("error: BLE dependency installation failed", file=sys.stderr)
        return result.returncode
    print(f"BLE environment ready: {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
