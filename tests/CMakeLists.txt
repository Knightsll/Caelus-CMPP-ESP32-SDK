cmake_minimum_required(VERSION 3.16)
project(cmpp_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(test_cmpp_core
    test_cmpp_core.c
    ../components/caelus_cmpp_core/src/cmpp.c
    ../components/caelus_cmpp_codec_json/src/cmpp_codec_json.c
    ../components/caelus_cmpp_adapter_mcp/src/cmpp_mcp_binding.c
    ../components/caelus_cmpp_adapter_mqtt/src/cmpp_mqtt_binding.c
    ../managed_components/espressif__cjson/cJSON/cJSON.c
)
target_include_directories(test_cmpp_core PRIVATE
    ../components/caelus_cmpp_core/include
    ../components/caelus_cmpp_codec_json/include
    ../components/caelus_cmpp_adapter_mcp/include
    ../components/caelus_cmpp_adapter_mqtt/include
    ../managed_components/espressif__cjson/cJSON
)
target_compile_definitions(test_cmpp_core PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(test_cmpp_core PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_cmpp_core PRIVATE Threads::Threads)

enable_testing()
add_test(NAME cmpp_core_contract COMMAND test_cmpp_core)

add_executable(test_cmpp_backends
    test_cmpp_backends.c
    ../components/caelus_cmpp_backend_pwm/src/cmpp_backend_pwm_logic.c
    ../components/caelus_cmpp_backend_relay/src/cmpp_backend_relay_logic.c
)
target_include_directories(test_cmpp_backends PRIVATE
    ../components/caelus_cmpp_core/include
    ../components/caelus_cmpp_backend_pwm/include
    ../components/caelus_cmpp_backend_relay/include
)
target_compile_options(test_cmpp_backends PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME cmpp_backend_contract COMMAND test_cmpp_backends)

add_executable(test_device_function
    test_device_function.c
    ../components/caelus_cmpp_core/src/cmpp.c
    ../components/caelus_device_function/src/test_board_device.c
)
target_include_directories(test_device_function PRIVATE
    ../components/caelus_cmpp_core/include
    ../components/caelus_cmpp_backend_pwm/include
    ../components/caelus_cmpp_backend_relay/include
    ../components/caelus_device_function/include
)
target_compile_definitions(test_device_function PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(test_device_function PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(test_device_function PRIVATE Threads::Threads)
add_test(NAME cmpp_device_function_contract COMMAND test_device_function)

add_executable(test_voice_wire
    test_voice_wire.c
    ../main/voice_wire.c
    ../managed_components/espressif__cjson/cJSON/cJSON.c
)
target_include_directories(test_voice_wire PRIVATE
    ../main
    ../managed_components/espressif__cjson/cJSON
)
target_compile_options(test_voice_wire PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME voice_wire_contract COMMAND test_voice_wire)

add_executable(test_voice_recovery test_voice_recovery.c ../main/voice_recovery_logic.c)
target_include_directories(test_voice_recovery PRIVATE ../main)
target_compile_options(test_voice_recovery PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME voice_recovery_contract COMMAND test_voice_recovery)

add_executable(test_voice_status test_voice_status.c)
target_include_directories(test_voice_status PRIVATE ../main)
target_compile_options(test_voice_status PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME voice_status_contract COMMAND test_voice_status)

add_executable(test_voice_lifecycle test_voice_lifecycle.c ../main/voice_lifecycle.c)
target_include_directories(test_voice_lifecycle PRIVATE ../main)
target_compile_options(test_voice_lifecycle PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME voice_lifecycle_contract COMMAND test_voice_lifecycle)

add_executable(test_voice_volume test_voice_volume.c ../main/voice_volume.c)
target_include_directories(test_voice_volume PRIVATE voice_stubs ../main)
target_compile_definitions(test_voice_volume PRIVATE CONFIG_CMPP_VOICE_OUTPUT_VOLUME=35)
target_compile_options(test_voice_volume PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME voice_volume_contract COMMAND test_voice_volume)

add_executable(test_voice_board test_voice_board.c ../main/voice_board.c
    ../main/voice_lifecycle.c ../main/voice_wire.c
    ../managed_components/espressif__cjson/cJSON/cJSON.c)
target_include_directories(test_voice_board PRIVATE voice_stubs ../main
    ../components/caelus_cmpp_core/include ../managed_components/espressif__cjson/cJSON)
target_compile_definitions(test_voice_board PRIVATE CONFIG_CMPP_VOICE_OUTPUT_VOLUME=35 CONFIG_CMPP_VOICE_HTTP_PORT=8081)
target_compile_options(test_voice_board PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME voice_board_contract COMMAND test_voice_board)

add_executable(test_voice_socket test_voice_socket.c)
target_include_directories(test_voice_socket PRIVATE ../main)
target_compile_options(test_voice_socket PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME voice_socket_contract COMMAND test_voice_socket)

add_executable(test_voice_upload_queue test_voice_upload_queue.c ../main/voice_upload_queue.c ../main/voice_lifecycle.c)
target_include_directories(test_voice_upload_queue PRIVATE ../main)
target_compile_options(test_voice_upload_queue PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME voice_upload_queue_contract COMMAND test_voice_upload_queue)
