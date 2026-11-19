#include "esp_log.h"

#include "board_config.h"
#include "cmpp/cmpp_sdk_esp32.h"

static const char *TAG = "cmpp_test_board";

void app_main(void)
{
    const cmpp_esp32_sdk_config_t sdk_config = {
        .profile_id = "caelus.test-board.v1",
        .server_name = "Caelus CMPP Test Board",
        .server_description = "Six PWM indicators and four relay outputs",
        .device_type = "cmpp-test-board",
        .max_capabilities = 4U,
        .idempotency_cache_size = 8U,
        .max_ttl_ms = 60000U,
        .persist_state_version = true,
    };
    cmpp_esp32_sdk_t *sdk = NULL;
    ESP_ERROR_CHECK(cmpp_esp32_sdk_create(&sdk_config, &sdk));

    board_runtime_t *board = NULL;
    ESP_ERROR_CHECK(board_runtime_create(cmpp_esp32_sdk_runtime(sdk), &board));
    ESP_ERROR_CHECK(cmpp_esp32_sdk_start(sdk));
    ESP_LOGI(TAG, "CMPP SDK %s started for %s", CMPP_ESP32_SDK_VERSION,
             cmpp_esp32_sdk_device_uid(sdk));
    (void)sdk;
    (void)board;
}
