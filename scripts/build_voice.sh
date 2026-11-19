#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export IDF_PATH="${IDF_PATH:-/home/autograph/esp/esp-idf-v6.0.2}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-/home/autograph/.espressif}"
export IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-$project_root/.esp-tools/python_env/idf6.0_py3.13_env}"
export IDF_PYTHON_CHECK_CONSTRAINTS=no
export ESP_IDF_VERSION=6.0.2
export PATH="$IDF_PYTHON_ENV_PATH/bin:$IDF_TOOLS_PATH/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:$PATH"

cd "$project_root"
"$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" \
    -B build-voice -D IDF_TARGET=esp32s3 \
    -D SDKCONFIG="$project_root/build-voice/sdkconfig" \
    -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.voice.defaults' "${@:-build}"
