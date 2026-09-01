cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET "esp32c3" CACHE STRING "ESP-IDF target")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
set(PROJECT_VER "0.3.0")
project(cmpp_test_board)
