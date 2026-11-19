#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "board_config.h"
#include "board_profile.h"
#if CONFIG_CMPP_VOICE_BOARD
#include "voice_board.h"
#include "voice_recovery.h"
#include "voice_status.h"
#endif
#include "cmpp/cmpp.h"
#include "cmpp/cmpp_platform_esp32.h"
#include "cmpp_device_server.h"
#include "cmpp_mqtt_client.h"
#include "device_identity.h"
#include "network.h"

static const char *TAG = "cmpp_test_board";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
#if !CONFIG_CMPP_VOICE_BOARD
    /* Legacy C3 behavior. Voice recovery must never silently erase identity or
     * its pending recovery journal on an NVS error. */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
#endif
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    init_nvs();
#if CONFIG_CMPP_VOICE_BOARD
    ESP_ERROR_CHECK(voice_recovery_boot());
    ESP_ERROR_CHECK(voice_status_init());
    ESP_ERROR_CHECK(voice_recovery_start());
#endif

    cmpp_esp32_platform_t *platform = NULL;
    ESP_ERROR_CHECK(cmpp_esp32_platform_create(NULL, &platform) == CMPP_OK
                        ? ESP_OK
                        : ESP_FAIL);

    device_identity_t identity = {0};
    ESP_ERROR_CHECK(device_identity_load(&identity));

    const cmpp_runtime_config_t runtime_config = {
        .device_id = identity.device_uid,
        .profile_id = BOARD_PROFILE_ID,
        .max_capabilities = 4U,
        .idempotency_cache_size = 8U,
        .max_ttl_ms = 60000U,
        .persist_state_version = true,
    };
    cmpp_runtime_t *runtime = NULL;
    ESP_ERROR_CHECK(cmpp_runtime_create(cmpp_esp32_platform_ops(platform),
                                        &runtime_config, &runtime) == CMPP_OK
                        ? ESP_OK
                        : ESP_FAIL);

    board_runtime_t *board = NULL;
#if !CONFIG_CMPP_VOICE_BOARD
    ESP_ERROR_CHECK(board_runtime_create(runtime, &board));
#endif

    ESP_ERROR_CHECK(network_connect());

#if CONFIG_CMPP_VOICE_BOARD
    /* Provisioning and recovery remain reachable when a codec is absent.
     * MCP/media still start only after audio initialization succeeds. */
    esp_err_t voice_err = board_runtime_create(runtime, &board);
    if (voice_err != ESP_OK) {
        ESP_LOGE(TAG, "Voice unavailable: %s; keeping network and management running", esp_err_to_name(voice_err));
        voice_status_update(VOICE_LED_FAULT, VOICE_LED_FAULT);
    }
#endif

    ESP_ERROR_CHECK(device_identity_load(&identity));

    ESP_LOGI(TAG, "Device UID: %s", identity.device_uid);
    ESP_LOGI(TAG, "Display name: %s", identity.display_name);
    ESP_LOGI(TAG, "mDNS hostname: %s.local", identity.hostname);

#if CONFIG_CMPP_VOICE_BOARD
    if (board) ESP_ERROR_CHECK(voice_board_start(board, &identity));
#endif
    ESP_ERROR_CHECK(cmpp_device_server_start(&identity, runtime));
    const esp_err_t mqtt_err = cmpp_mqtt_client_start(&identity, runtime);
    if (mqtt_err != ESP_OK) {
        ESP_LOGW(TAG, "Optional MQTT transport did not start: %s",
                 esp_err_to_name(mqtt_err));
    }

    ESP_LOGI(TAG, "Ready: http://%s.local:%d/mcp",
             identity.hostname, CONFIG_CMPP_BOARD_HTTP_PORT);
    (void)board;
}
