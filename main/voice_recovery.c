/* SPDX-License-Identifier: Apache-2.0 */
#include "voice_recovery.h"
#include "voice_recovery_logic.h"
#include "voice_status.h"
#include "device_identity.h"
#include "network.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define MAINTENANCE_GPIO 17 /* SW1 net IO36 is U4 GPIO17, not GPIO36. */
#define INTENT_NAMESPACE "cmpp_recovery"
#define INTENT_KEY "action"
static const char *TAG = "voice_recovery";

static esp_err_t read_intent(uint8_t *action)
{
    *action = VOICE_RECOVERY_NONE;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(INTENT_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_get_u8(handle, INTENT_KEY, action);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    return *action <= VOICE_RECOVERY_FACTORY ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t write_intent(uint8_t action)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(INTENT_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, INTENT_KEY, action);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    uint8_t saved = 0;
    if (err == ESP_OK) err = read_intent(&saved);
    return err == ESP_OK && saved != action ? ESP_ERR_INVALID_STATE : err;
}

static int mark_unpaired(void *context)
{
    (void)context;
    return device_identity_reopen_pairing();
}

static int reset_wifi(void *context)
{
    (void)context;
    esp_err_t err = network_prepare();
    return err == ESP_OK ? esp_wifi_restore() : err;
}

static int erase_namespace(void *context, const char *name)
{
    (void)context;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(name, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static int finish(void *context)
{
    (void)context;
    return write_intent(VOICE_RECOVERY_NONE);
}

static void report(voice_recovery_event_t event)
{
    switch (event) {
    case RECOVERY_NETWORK_ARMED:
        ESP_LOGW(TAG, "SW1: release before 12 s for Wi-Fi setup; identity retained"); break;
    case RECOVERY_FACTORY_ARMED:
        ESP_LOGW(TAG, "SW1: release before 20 s to request factory confirmation (no erase yet)"); break;
    case RECOVERY_CONFIRM_PROMPT:
        ESP_LOGW(TAG, "Factory pending: within 10 s press SW1 for >=3 s then release; otherwise cancel"); break;
    case RECOVERY_CONFIRM_ARMED:
        ESP_LOGW(TAG, "Release SW1 before confirmation deadline to erase device identity/settings"); break;
    case RECOVERY_CANCELLED:
        ESP_LOGI(TAG, "Maintenance cancelled or not requested; settings unchanged"); break;
    default: break;
    }
}

esp_err_t voice_recovery_boot(void)
{
    /* Keep battery power latched during the boot-only maintenance window. */
    gpio_config_t power = {.pin_bit_mask = 1ULL << 38, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&power), TAG, "Power hold setup failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(38, 1), TAG, "Power hold failed");
    gpio_config_t key = {.pin_bit_mask = 1ULL << MAINTENANCE_GPIO,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&key), TAG, "SW1 setup failed");
    uint8_t pending;
    ESP_RETURN_ON_ERROR(read_intent(&pending), TAG, "Cannot read recovery intent");
    if (!pending) {
        voice_recovery_gesture_t gesture = {0};
        bool candidate = gpio_get_level(MAINTENANCE_GPIO) == 0;
        bool stable = candidate;
        int64_t changed = esp_timer_get_time();
        const int64_t started = changed;
        /* Debounce both boot selection and subsequent edges for 50 ms. */
        do {
            bool raw = gpio_get_level(MAINTENANCE_GPIO) == 0;
            int64_t now = esp_timer_get_time();
            if (gesture.phase == RECOVERY_WAIT_BOOT_KEY && now - started > 500000) {
                report(RECOVERY_CANCELLED); /* No stable boot key: fail closed. */
                return ESP_OK;
            }
            if (raw != candidate) { candidate = raw; changed = now; }
            if (now - changed >= 50000) {
                stable = candidate;
                if (gesture.phase == RECOVERY_WAIT_BOOT_KEY && stable)
                    ESP_LOGW(TAG, "Boot maintenance active: SW1 GPIO17; release before 5 s to cancel");
                report(voice_recovery_step(&gesture, stable, (uint64_t)(now / 1000)));
            } else if (gesture.phase != RECOVERY_WAIT_BOOT_KEY) {
                report(voice_recovery_step(&gesture, stable, (uint64_t)(now / 1000)));
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        } while (gesture.phase != RECOVERY_DONE);
        pending = (uint8_t)gesture.action;
        if (!pending) return ESP_OK;
        ESP_RETURN_ON_ERROR(write_intent(pending), TAG, "Could not persist confirmed intent; no cleanup started");
    } else {
        ESP_LOGW(TAG, "Resuming committed recovery intent %u before starting services", pending);
    }
    const voice_recovery_ops_t ops = {
        .mark_unpaired = mark_unpaired, .reset_wifi = reset_wifi,
        .erase_namespace = erase_namespace, .finish = finish,
    };
    ESP_LOGW(TAG, "Applying %s", pending == VOICE_RECOVERY_NETWORK ? "Wi-Fi setup reset" : "factory reset");
    ESP_RETURN_ON_ERROR(voice_recovery_apply(pending, &ops), TAG,
                        "Recovery incomplete; intent retained, services blocked");
    ESP_LOGW(TAG, "Recovery complete; continuing into BLE/SoftAP setup");
    return ESP_OK;
}

static void runtime_key_task(void *arg)
{
    (void)arg;
    voice_recovery_runtime_t key = {0};
    for (;;) {
        voice_recovery_event_t event = voice_recovery_runtime_step(&key,
            gpio_get_level(MAINTENANCE_GPIO) == 0,
            (uint64_t)(esp_timer_get_time() / 1000));
        if (event == RECOVERY_NETWORK_ARMED) {
            voice_status_update(VOICE_LED_MAINTENANCE, VOICE_LED_MAINTENANCE);
            ESP_LOGW(TAG, "Runtime SW1: release before 12 s to restart into Wi-Fi setup");
        } else if (event == RECOVERY_CANCELLED) {
            voice_status_update(VOICE_LED_MAINTENANCE, 0);
            ESP_LOGI(TAG, "Runtime SW1 cancelled; settings unchanged");
        } else if (event == RECOVERY_NETWORK_SELECTED) {
            /* Do not change identity or Wi-Fi underneath running services.
             * Commit the existing recovery journal, then apply it next boot.
             * Restart also restores BLE memory released after pairing. */
            esp_err_t err = write_intent(VOICE_RECOVERY_NETWORK);
            if (err == ESP_OK) {
                ESP_LOGW(TAG, "Runtime SW1: recovery intent committed; restarting into setup");
                esp_restart();
            }
            voice_status_update(VOICE_LED_MAINTENANCE | VOICE_LED_FAULT, VOICE_LED_FAULT);
            ESP_LOGE(TAG, "Cannot commit recovery intent; no restart: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t voice_recovery_start(void)
{
    if (xTaskCreate(runtime_key_task, "voice_recovery", 3072, NULL, 1, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Runtime SW1 enabled: GPIO17, hold 5 to <12 s then release for Wi-Fi setup");
    return ESP_OK;
}
