#include "device_identity.h"

#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "sha/sha_core.h"

/* Keep the original namespace so reflashing does not rotate an already paired
 * board's stable UID or Bearer credential. */
#define IDENTITY_NAMESPACE "mcp_light"
#define SALT_KEY "uid_salt"
#define DISPLAY_NAME_KEY "display_name"
#define HOSTNAME_KEY "hostname"
#define AUTH_TOKEN_KEY "auth_token"
#define PAIRING_COMPLETE_KEY "paired"
#define SALT_SIZE 16

static esp_err_t load_or_create_auth_token(
    char auth_token[DEVICE_AUTH_TOKEN_MAX_LEN])
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(IDENTITY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = DEVICE_AUTH_TOKEN_MAX_LEN;
    err = nvs_get_str(handle, AUTH_TOKEN_KEY, auth_token, &size);
    if (err == ESP_OK && size == DEVICE_AUTH_TOKEN_MAX_LEN) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NVS_INVALID_LENGTH &&
        err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    uint8_t random_bytes[DEVICE_AUTH_TOKEN_BYTES];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (size_t i = 0; i < sizeof(random_bytes); ++i) {
        snprintf(&auth_token[i * 2], 3, "%02x", random_bytes[i]);
    }
    auth_token[DEVICE_AUTH_TOKEN_MAX_LEN - 1] = '\0';
    memset(random_bytes, 0, sizeof(random_bytes));

    err = nvs_set_str(handle, AUTH_TOKEN_KEY, auth_token);
    if (err == ESP_OK) {
        /* A newly generated credential must be delivered through the setup
         * portal before the device may boot directly into MCP mode. */
        err = nvs_set_u8(handle, PAIRING_COMPLETE_KEY, 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t load_or_create_salt(uint8_t salt[SALT_SIZE])
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(IDENTITY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t size = SALT_SIZE;
    err = nvs_get_blob(handle, SALT_KEY, salt, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND || size != SALT_SIZE) {
        esp_fill_random(salt, SALT_SIZE);
        err = nvs_set_blob(handle, SALT_KEY, salt, SALT_SIZE);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
    }

    nvs_close(handle);
    return err;
}

static esp_err_t load_display_name(char *display_name, size_t display_name_size,
                                   const uint8_t mac[6])
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(IDENTITY_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = display_name_size;
        err = nvs_get_str(handle, DISPLAY_NAME_KEY, display_name, &size);
        nvs_close(handle);
        if (err == ESP_OK) {
            if (display_name[0] != '\0') {
                return ESP_OK;
            }
        } else if (err != ESP_ERR_NVS_NOT_FOUND &&
                   err != ESP_ERR_NVS_INVALID_LENGTH) {
            return err;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    const int written = snprintf(
        display_name, display_name_size, "Caelus Board %02X%02X%02X",
        mac[3], mac[4], mac[5]);
    return written >= 0 && (size_t)written < display_name_size
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

static esp_err_t load_hostname(char *hostname, size_t hostname_size,
                               const uint8_t mac[6])
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(IDENTITY_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = hostname_size;
        err = nvs_get_str(handle, HOSTNAME_KEY, hostname, &size);
        nvs_close(handle);
        if (err == ESP_OK) {
            if (hostname[0] != '\0') {
                return ESP_OK;
            }
        } else if (err != ESP_ERR_NVS_NOT_FOUND &&
                   err != ESP_ERR_NVS_INVALID_LENGTH) {
            return err;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    const int written = snprintf(hostname, hostname_size, "%s-%02x%02x%02x",
                                 CONFIG_CMPP_BOARD_HOSTNAME_PREFIX,
                                 mac[3], mac[4], mac[5]);
    return written >= 0 && (size_t)written < hostname_size
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

esp_err_t device_identity_set_setup(const char *display_name,
                                    const char *hostname)
{
    if (!display_name || display_name[0] == '\0' ||
        strlen(display_name) >= DEVICE_DISPLAY_NAME_MAX_LEN ||
        !hostname || hostname[0] == '\0' ||
        strlen(hostname) >= DEVICE_HOSTNAME_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    char auth_token[DEVICE_AUTH_TOKEN_MAX_LEN] = {0};
    esp_err_t err = load_or_create_auth_token(auth_token);
    memset(auth_token, 0, sizeof(auth_token));
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(IDENTITY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, DISPLAY_NAME_KEY, display_name);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, HOSTNAME_KEY, hostname);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, PAIRING_COMPLETE_KEY, 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t device_identity_complete_pairing(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(IDENTITY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, PAIRING_COMPLETE_KEY, 1);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t device_identity_pairing_complete(bool *complete)
{
    if (!complete) {
        return ESP_ERR_INVALID_ARG;
    }
    *complete = false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(IDENTITY_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t value = 0;
    err = nvs_get_u8(handle, PAIRING_COMPLETE_KEY, &value);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    *complete = value == 1;
    return ESP_OK;
}

esp_err_t device_identity_load(device_identity_t *identity)
{
    if (!identity) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6];
    uint8_t salt[SALT_SIZE];
    uint8_t input[SALT_SIZE + sizeof(mac)];
    uint8_t digest[32];

    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        return err;
    }
    err = load_or_create_salt(salt);
    if (err != ESP_OK) {
        return err;
    }

    memcpy(input, salt, SALT_SIZE);
    memcpy(input + SALT_SIZE, mac, sizeof(mac));
    esp_sha(SHA2_256, input, sizeof(input), digest);

    int written = snprintf(identity->device_uid, sizeof(identity->device_uid),
                           "hw_%02x%02x%02x%02x%02x%02x%02x%02x",
                           digest[0], digest[1], digest[2], digest[3],
                           digest[4], digest[5], digest[6], digest[7]);
    if (written < 0 || (size_t)written >= sizeof(identity->device_uid)) {
        return ESP_ERR_INVALID_SIZE;
    }

    err = load_hostname(identity->hostname, sizeof(identity->hostname), mac);
    if (err != ESP_OK) {
        return err;
    }
    err = load_display_name(identity->display_name,
                            sizeof(identity->display_name), mac);
    if (err != ESP_OK) {
        return err;
    }
    return load_or_create_auth_token(identity->auth_token);
}
