#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${project_dir}/build-host-tests"

cmake -S "${project_dir}/tests" -B "${build_dir}" -G Ninja
cmake --build "${build_dir}"
ctest --test-dir "${build_dir}" --output-on-failure
