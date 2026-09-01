#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
port="${ESPPORT:-/dev/ttyUSB0}"

export IDF_PATH=/home/autograph/esp/esp-idf-v6.0.2
export IDF_TOOLS_PATH=/home/autograph/.espressif
export IDF_PYTHON_ENV_PATH="$project_root/.esp-tools/python_env/idf6.0_py3.13_env"
export IDF_PYTHON_CHECK_CONSTRAINTS=no
export ESP_IDF_VERSION=6.0.2
export PATH="$IDF_PYTHON_ENV_PATH/bin:/home/autograph/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin:/home/autograph/.espressif/tools/riscv32-esp-elf-gdb/17.1_20260402/riscv32-esp-elf-gdb/bin:$PATH"

cd "$project_root"
"$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" \
    -p "$port" flash monitor
