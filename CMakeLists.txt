cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET "esp32c3" CACHE STRING "ESP-IDF target")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
set(PROJECT_VER "0.3.0")
if(IDF_TARGET STREQUAL "esp32s3")
    set(PROJECT_VER "0.5.0-dev")
    set(DEPENDENCIES_LOCK "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.voice.lock")
    idf_build_set_property(DEPENDENCIES_LOCK "${DEPENDENCIES_LOCK}")
    project(cmpp_voice_terminal)
    # Keep the pinned MCP transport at its existing optimization level: GCC 15
    # flags two already-zero-terminated strncpy sites in SDK 2.0.1 at -O2.
    # Audio/DSP components still use PERF; don't suppress warnings or edit SDK.
    idf_component_get_property(cmpp_mcp_lib espressif__mcp-c-sdk COMPONENT_LIB)
    target_compile_options(${cmpp_mcp_lib} PRIVATE -Og)
else()
    project(cmpp_test_board)
endif()
