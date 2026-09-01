#include "network.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "device_identity.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "dns_server.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

#define WIFI_CONNECTED_BIT        BIT0
#define WIFI_CONNECT_FAILED_BIT   BIT1
#define PORTAL_CONFIGURED_BIT     BIT2
#define WIFI_CONNECT_RETRIES      5
#define PORTAL_SCAN_RESULTS       16
#define PORTAL_REQUEST_MAX_BYTES  512
#define PORTAL_RESPONSE_GRACE_MS  1000
#define WIFI_IDENTITY_NAMESPACE   "cmpp_wifi"
#define WIFI_IDENTITY_KEY         "sta_config"

extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char portal_html_end[] asm("_binary_portal_html_end");

static const char *TAG = "cmpp_board_wifi";
static EventGroupHandle_t s_wifi_events;
static httpd_handle_t s_portal_server;
static dns_server_handle_t s_dns_server;
static esp_netif_t *s_ap_netif;
static char s_ip_address[16] = "0.0.0.0";
static char s_portal_ssid[33];
static char s_suggested_display_name[DEVICE_DISPLAY_NAME_MAX_LEN];
static char s_suggested_hostname[DEVICE_HOSTNAME_MAX_LEN];
static char s_captive_portal_uri[32];
static bool s_portal_active;
static bool s_resume_pairing;
static bool s_connect_requested;
static int s_retry_count;

static esp_err_t make_portal_ssid(void)
{
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        return err;
    }

    const int written = snprintf(
        s_portal_ssid, sizeof(s_portal_ssid), "%s-%02X%02X%02X",
        CONFIG_CMPP_BOARD_PROVISIONING_SSID_PREFIX,
        mac[3], mac[4], mac[5]);
    if (written < 0 || (size_t)written >= sizeof(s_portal_ssid)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int name_written = snprintf(
        s_suggested_display_name, sizeof(s_suggested_display_name),
        "Caelus Board %02X%02X%02X", mac[3], mac[4], mac[5]);
    if (name_written < 0 ||
        (size_t)name_written >= sizeof(s_suggested_display_name)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int hostname_written = snprintf(
        s_suggested_hostname, sizeof(s_suggested_hostname),
        "%s-%02x%02x%02x", CONFIG_CMPP_BOARD_HOSTNAME_PREFIX,
        mac[3], mac[4], mac[5]);
    if (hostname_written < 0 ||
        (size_t)hostname_written >= sizeof(s_suggested_hostname)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static bool display_name_valid(const char *display_name)
{
    if (!display_name) {
        return false;
    }
    const size_t length = strlen(display_name);
    if (length == 0 || length >= DEVICE_DISPLAY_NAME_MAX_LEN ||
        display_name[0] == ' ' || display_name[length - 1] == ' ') {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char value = (unsigned char)display_name[i];
        if (value < 0x20 || value == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool hostname_valid(const char *hostname)
{
    if (!hostname) {
        return false;
    }
    const size_t length = strlen(hostname);
    if (length == 0 || length >= DEVICE_HOSTNAME_MAX_LEN ||
        !((hostname[0] >= 'a' && hostname[0] <= 'z') ||
          (hostname[0] >= '0' && hostname[0] <= '9')) ||
        !((hostname[length - 1] >= 'a' && hostname[length - 1] <= 'z') ||
          (hostname[length - 1] >= '0' && hostname[length - 1] <= '9'))) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const char value = hostname[i];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '-')) {
            return false;
        }
    }
    return true;
}

static esp_err_t load_persistent_wifi_config(wifi_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_IDENTITY_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t size = sizeof(config->sta);
    err = nvs_get_blob(handle, WIFI_IDENTITY_KEY, &config->sta, &size);
    nvs_close(handle);
    if (err == ESP_OK && size != sizeof(config->sta)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

static esp_err_t save_persistent_wifi_config(const wifi_config_t *config)
{
    if (!config || config->sta.ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_IDENTITY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, WIFI_IDENTITY_KEY, &config->sta,
                       sizeof(config->sta));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static bool has_saved_wifi_credentials(void)
{
    wifi_config_t saved = {0};
    if (load_persistent_wifi_config(&saved) == ESP_OK &&
        saved.sta.ssid[0] != '\0') {
        return true;
    }
    return esp_wifi_get_config(WIFI_IF_STA, &saved) == ESP_OK &&
           saved.sta.ssid[0] != '\0';
}

static void network_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            if (!s_portal_active || s_connect_requested) {
                esp_wifi_connect();
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            strlcpy(s_ip_address, "0.0.0.0", sizeof(s_ip_address));
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);

            if (s_portal_active) {
                if (!s_connect_requested) {
                    return;
                }
                if (++s_retry_count < WIFI_CONNECT_RETRIES) {
                    ESP_LOGW(TAG, "Wi-Fi test failed; retry %d/%d",
                             s_retry_count, WIFI_CONNECT_RETRIES);
                    esp_wifi_connect();
                } else {
                    s_connect_requested = false;
                    xEventGroupSetBits(s_wifi_events,
                                       WIFI_CONNECT_FAILED_BIT);
                }
            } else {
                ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
                esp_wifi_connect();
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        snprintf(s_ip_address, sizeof(s_ip_address), IPSTR,
                 IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t send_json(httpd_req_t *req, const char *status,
                           const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t redirect_to_portal(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", s_captive_portal_uri);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "Open the Caelus setup portal",
                           HTTPD_RESP_USE_STRLEN);
}

static bool request_uses_portal_host(httpd_req_t *req)
{
    char host[48] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) !=
        ESP_OK) {
        return false;
    }

    const char *portal_host = s_captive_portal_uri + strlen("http://");
    const size_t portal_host_length = strlen(portal_host);
    return strncmp(host, portal_host, portal_host_length) == 0 &&
           (host[portal_host_length] == '\0' ||
            strcmp(host + portal_host_length, ":80") == 0);
}

static esp_err_t portal_root_handler(httpd_req_t *req)
{
    if (!request_uses_portal_host(req)) {
        return redirect_to_portal(req);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(
        req, portal_html_start, portal_html_end - portal_html_start);
}

static const char *auth_mode_name(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
        return "wpa";
    case WIFI_AUTH_WPA2_PSK:
        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa/wpa2";
    case WIFI_AUTH_WPA3_PSK:
        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2/wpa3";
    case WIFI_AUTH_OWE:
        return "owe";
    default:
        return "secured";
    }
}

static esp_err_t portal_networks_handler(httpd_req_t *req)
{
    if (s_connect_requested) {
        return send_json(req, "409 Conflict",
                         "{\"ok\":false,\"error\":\"connection_in_progress\"}");
    }

    wifi_ap_record_t records[PORTAL_SCAN_RESULTS] = {0};
    uint16_t count = PORTAL_SCAN_RESULTS;
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&count, records);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        return send_json(req, "503 Service Unavailable",
                         "{\"ok\":false,\"error\":\"scan_failed\"}");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"error\":\"out_of_memory\"}");
    }
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    if (!networks) {
        cJSON_Delete(root);
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"error\":\"out_of_memory\"}");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "suggested_name",
                           s_suggested_display_name);
    cJSON_AddStringToObject(root, "suggested_hostname",
                           s_suggested_hostname);

    char seen[PORTAL_SCAN_RESULTS][33] = {{0}};
    size_t seen_count = 0;
    for (uint16_t i = 0; i < count; ++i) {
        char ssid[33] = {0};
        memcpy(ssid, records[i].ssid, sizeof(records[i].ssid));
        ssid[sizeof(ssid) - 1] = '\0';
        if (ssid[0] == '\0') {
            continue;
        }

        bool duplicate = false;
        for (size_t j = 0; j < seen_count; ++j) {
            if (strcmp(seen[j], ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        strlcpy(seen[seen_count++], ssid, sizeof(seen[0]));

        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            return send_json(req, "500 Internal Server Error",
                             "{\"ok\":false,\"error\":\"out_of_memory\"}");
        }
        cJSON_AddStringToObject(item, "ssid", ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddStringToObject(
            item, "security", auth_mode_name(records[i].authmode));
        cJSON_AddItemToArray(networks, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"error\":\"out_of_memory\"}");
    }
    err = send_json(req, "200 OK", json);
    cJSON_free(json);
    return err;
}

static esp_err_t receive_request_body(httpd_req_t *req, char *buffer,
                                      size_t buffer_size)
{
    if (req->content_len <= 0 ||
        req->content_len >= buffer_size ||
        req->content_len > PORTAL_REQUEST_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    while (received < (size_t)req->content_len) {
        const int result = httpd_req_recv(
            req, buffer + received, req->content_len - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += result;
    }
    buffer[received] = '\0';
    return ESP_OK;
}

static char *build_configured_response(const device_identity_t *identity)
{
    char endpoint[128];
    char authorization[sizeof("Bearer ") + DEVICE_AUTH_TOKEN_MAX_LEN];
    const int written = snprintf(
        endpoint, sizeof(endpoint), "http://%s.local:%d/mcp",
        identity->hostname, CONFIG_CMPP_BOARD_HTTP_PORT);
    if (written < 0 || (size_t)written >= sizeof(endpoint)) {
        return NULL;
    }
    const int auth_written = snprintf(
        authorization, sizeof(authorization), "Bearer %s",
        identity->auth_token);
    if (auth_written < 0 || (size_t)auth_written >= sizeof(authorization)) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *device = cJSON_CreateObject();
    cJSON *connection = cJSON_CreateObject();
    cJSON *headers = cJSON_CreateObject();
    if (!root || !device || !connection || !headers) {
        cJSON_Delete(root);
        cJSON_Delete(device);
        cJSON_Delete(connection);
        cJSON_Delete(headers);
        return NULL;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "message", "configured");
    cJSON_AddStringToObject(device, "device_uid", identity->device_uid);
    cJSON_AddStringToObject(device, "display_name", identity->display_name);
    cJSON_AddStringToObject(device, "hostname", identity->hostname);
    cJSON_AddStringToObject(device, "endpoint", endpoint);
    cJSON_AddStringToObject(device, "discovery", "_cmpp._tcp.local");
    cJSON_AddStringToObject(device, "auth_type", "bearer");
    cJSON_AddStringToObject(device, "auth_token", identity->auth_token);
    cJSON_AddItemToObject(root, "device", device);

    cJSON_AddStringToObject(connection, "transport", "streamable_http");
    cJSON_AddStringToObject(connection, "url", endpoint);
    cJSON_AddStringToObject(headers, "Authorization", authorization);
    cJSON_AddItemToObject(connection, "headers", headers);
    cJSON_AddItemToObject(root, "connection", connection);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t portal_pairing_handler(httpd_req_t *req)
{
    if (!s_resume_pairing) {
        return send_json(req, "404 Not Found",
                         "{\"ok\":false,\"error\":\"no_pending_pairing\"}");
    }

    device_identity_t identity = {0};
    esp_err_t err = device_identity_load(&identity);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not load pending pairing: %s",
                 esp_err_to_name(err));
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"error\":\"identity_failed\"}");
    }

    char *response_json = build_configured_response(&identity);
    memset(identity.auth_token, 0, sizeof(identity.auth_token));
    if (!response_json) {
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"error\":\"out_of_memory\"}");
    }

    err = send_json(req, "200 OK", response_json);
    cJSON_free(response_json);
    return err;
}

static esp_err_t stop_setup_hotspot(void)
{
    ESP_LOGI(TAG, "Closing setup hotspot");

    /*
     * Disable AP mode first so the provisioning SSID stops being advertised.
     * The HTTP response has already been queued before network_connect() calls
     * this function.
     */
    s_portal_active = false;
    s_connect_requested = false;
    esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);

    if (s_dns_server) {
        stop_dns_server(s_dns_server);
        s_dns_server = NULL;
    }

    esp_err_t http_err = ESP_OK;
    if (s_portal_server) {
        http_err = httpd_stop(s_portal_server);
        s_portal_server = NULL;
    }

    ESP_RETURN_ON_ERROR(mode_err, TAG,
                        "Could not switch from setup AP to station mode");
    ESP_RETURN_ON_ERROR(http_err, TAG,
                        "Could not stop setup HTTP server");

    wifi_mode_t mode = WIFI_MODE_NULL;
    ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&mode), TAG,
                        "Could not verify Wi-Fi mode");
    ESP_RETURN_ON_FALSE(mode == WIFI_MODE_STA, ESP_ERR_INVALID_STATE, TAG,
                        "Setup hotspot is still active (mode=%d)", mode);

    ESP_LOGI(TAG, "Setup hotspot closed");
    return ESP_OK;
}

static esp_err_t portal_configure_handler(httpd_req_t *req)
{
    char body[PORTAL_REQUEST_MAX_BYTES + 1] = {0};
    if (receive_request_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json(req, "400 Bad Request",
                         "{\"ok\":false,\"error\":\"invalid_request\"}");
    }

    cJSON *root = cJSON_Parse(body);
    const cJSON *ssid_item =
        cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *password_item =
        cJSON_GetObjectItemCaseSensitive(root, "password");
    const cJSON *display_name_item =
        cJSON_GetObjectItemCaseSensitive(root, "display_name");
    const cJSON *hostname_item =
        cJSON_GetObjectItemCaseSensitive(root, "hostname");
    if (!cJSON_IsObject(root) ||
        !cJSON_IsString(display_name_item) ||
        !display_name_valid(display_name_item->valuestring) ||
        !cJSON_IsString(hostname_item) ||
        !hostname_valid(hostname_item->valuestring) ||
        !cJSON_IsString(ssid_item) || !ssid_item->valuestring ||
        ssid_item->valuestring[0] == '\0' ||
        strlen(ssid_item->valuestring) > 32 ||
        (password_item &&
         (!cJSON_IsString(password_item) || !password_item->valuestring)) ||
        (password_item &&
         strlen(password_item->valuestring) > 64)) {
        cJSON_Delete(root);
        memset(body, 0, sizeof(body));
        return send_json(req, "400 Bad Request",
                         "{\"ok\":false,\"error\":\"invalid_credentials\"}");
    }

    wifi_config_t wifi_config = {0};
    const size_t ssid_length = strlen(ssid_item->valuestring);
    memcpy(wifi_config.sta.ssid, ssid_item->valuestring, ssid_length);
    if (cJSON_IsString(password_item) && password_item->valuestring) {
        const size_t password_length = strlen(password_item->valuestring);
        memcpy(wifi_config.sta.password, password_item->valuestring,
               password_length);
    }
    wifi_config.sta.threshold.authmode =
        wifi_config.sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_LOGI(TAG, "Testing Wi-Fi credentials for SSID '%s'",
             ssid_item->valuestring);
    xEventGroupClearBits(s_wifi_events,
                         WIFI_CONNECTED_BIT | WIFI_CONNECT_FAILED_BIT);
    s_retry_count = 0;
    s_connect_requested = true;
    esp_wifi_disconnect();

    esp_err_t err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        s_connect_requested = false;
        cJSON_Delete(root);
        memset(body, 0, sizeof(body));
        memset(&wifi_config, 0, sizeof(wifi_config));
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"error\":\"wifi_start_failed\"}");
    }

    const EventBits_t result = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT | WIFI_CONNECT_FAILED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if (!(result & WIFI_CONNECTED_BIT)) {
        s_connect_requested = false;
        esp_wifi_disconnect();
        cJSON_Delete(root);
        memset(body, 0, sizeof(body));
        memset(&wifi_config, 0, sizeof(wifi_config));
        return send_json(req, "422 Unprocessable Content",
                         "{\"ok\":false,\"error\":\"wifi_connection_failed\"}");
    }

    err = device_identity_set_setup(display_name_item->valuestring,
                                    hostname_item->valuestring);
    if (err == ESP_OK) {
        err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (err == ESP_OK) {
        err = save_persistent_wifi_config(&wifi_config);
    }
    cJSON_Delete(root);
    memset(body, 0, sizeof(body));
    memset(&wifi_config, 0, sizeof(wifi_config));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save setup configuration: %s",
                 esp_err_to_name(err));
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"error\":\"save_failed\"}");
    }

    device_identity_t identity = {0};
    err = device_identity_load(&identity);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not load configured identity: %s",
                 esp_err_to_name(err));
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"error\":\"identity_failed\"}");
    }
    char *response_json = build_configured_response(&identity);
    if (!response_json) {
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"error\":\"out_of_memory\"}");
    }

    ESP_LOGI(TAG, "Wi-Fi verified and saved in NVS, IP=%s", s_ip_address);
    ESP_LOGI(TAG, "MCP endpoint after setup: http://%s.local:%d/mcp",
             identity.hostname, CONFIG_CMPP_BOARD_HTTP_PORT);
    s_resume_pairing = true;
    const esp_err_t response_err = send_json(req, "200 OK", response_json);
    cJSON_free(response_json);
    return response_err;
}

static esp_err_t portal_complete_handler(httpd_req_t *req)
{
    esp_err_t err = device_identity_complete_pairing();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not complete device pairing: %s",
                 esp_err_to_name(err));
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"error\":\"pairing_save_failed\"}");
    }
    s_resume_pairing = false;

    const esp_err_t response_err = send_json(
        req, "200 OK", "{\"ok\":true,\"message\":\"pairing_complete\"}");
    if (response_err == ESP_OK) {
        xEventGroupSetBits(s_wifi_events, PORTAL_CONFIGURED_BIT);
    }
    return response_err;
}

static esp_err_t portal_404_handler(httpd_req_t *req,
                                    httpd_err_code_t error)
{
    (void)error;
    return redirect_to_portal(req);
}

static esp_err_t start_portal_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_portal_server, &config), TAG,
                        "Setup web server failed");

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = portal_root_handler,
    };
    const httpd_uri_t networks = {
        .uri = "/api/networks",
        .method = HTTP_GET,
        .handler = portal_networks_handler,
    };
    const httpd_uri_t configure = {
        .uri = "/api/configure",
        .method = HTTP_POST,
        .handler = portal_configure_handler,
    };
    const httpd_uri_t complete = {
        .uri = "/api/complete",
        .method = HTTP_POST,
        .handler = portal_complete_handler,
    };
    const httpd_uri_t pairing = {
        .uri = "/api/pairing",
        .method = HTTP_GET,
        .handler = portal_pairing_handler,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_portal_server, &root),
                        TAG, "Portal root registration failed");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(s_portal_server, &networks),
        TAG, "Portal scan registration failed");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(s_portal_server, &configure),
        TAG, "Portal configure registration failed");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(s_portal_server, &complete),
        TAG, "Portal pairing completion registration failed");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(s_portal_server, &pairing),
        TAG, "Pending pairing registration failed");
    return httpd_register_err_handler(
        s_portal_server, HTTPD_404_NOT_FOUND, portal_404_handler);
}

static esp_err_t configure_dhcp_captive_portal(void)
{
    esp_netif_ip_info_t ip_info;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_ap_netif, &ip_info), TAG,
                        "Could not read setup hotspot IP");

    char ip[16];
    inet_ntoa_r(ip_info.ip.addr, ip, sizeof(ip));
    const int written = snprintf(
        s_captive_portal_uri, sizeof(s_captive_portal_uri),
        "http://%s", ip);
    if (written < 0 || (size_t)written >= sizeof(s_captive_portal_uri)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_ap_netif));
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(
        s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
        s_captive_portal_uri, strlen(s_captive_portal_uri)),
        TAG, "DHCP captive portal option failed");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(s_ap_netif), TAG,
                        "DHCP server restart failed");
    return ESP_OK;
}

static esp_err_t start_setup_hotspot(void)
{
    ESP_RETURN_ON_ERROR(make_portal_ssid(), TAG,
                        "Could not create setup SSID");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "Could not select temporary Wi-Fi storage");

    wifi_config_t ap_config = {0};
    ap_config.ap.ssid_len = strlen(s_portal_ssid);
    memcpy(ap_config.ap.ssid, s_portal_ssid, ap_config.ap.ssid_len);
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = 1;

    s_portal_active = true;
    s_connect_requested = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG,
                        "Could not set setup AP mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG,
                        "Could not configure setup AP");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG,
                        "Could not start setup AP");
    ESP_RETURN_ON_ERROR(configure_dhcp_captive_portal(), TAG,
                        "Could not configure captive portal DHCP");
    ESP_RETURN_ON_ERROR(start_portal_server(), TAG,
                        "Could not start setup page");

    dns_server_config_t dns_config =
        DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns_server = start_dns_server(&dns_config);
    ESP_RETURN_ON_FALSE(s_dns_server, ESP_FAIL, TAG,
                        "Could not start captive portal DNS");

    ESP_LOGW(TAG, "First-time setup hotspot: %s", s_portal_ssid);
    ESP_LOGW(TAG, "Open %s if the setup page does not appear automatically",
             s_captive_portal_uri);
    return ESP_OK;
}

esp_err_t network_connect(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_ap_netif, ESP_ERR_NO_MEM, TAG,
                        "Could not create setup AP interface");

    const wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG,
                        "Could not select persistent Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, network_event_handler, NULL),
        TAG, "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, network_event_handler, NULL),
        TAG, "IP event registration failed");

    wifi_config_t persistent_wifi = {0};
    err = load_persistent_wifi_config(&persistent_wifi);
    if (err == ESP_OK) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &persistent_wifi),
                            TAG, "Could not restore saved Wi-Fi config");
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Saved Wi-Fi config is unreadable: %s",
                 esp_err_to_name(err));
    }

    bool pairing_complete = false;
    ESP_RETURN_ON_ERROR(device_identity_pairing_complete(&pairing_complete),
                        TAG, "Could not read pairing state");
    const bool has_saved_wifi = has_saved_wifi_credentials();
    ESP_LOGI(TAG, "Persistent setup state: wifi=%s, pairing=%s",
             has_saved_wifi ? "saved" : "missing",
             pairing_complete ? "complete" : "pending");
    if (has_saved_wifi && pairing_complete) {
        ESP_LOGI(TAG, "Saved Wi-Fi found; starting station mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                            "Could not set station mode");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Could not start Wi-Fi");
        xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);
    } else {
        s_resume_pairing = has_saved_wifi && !pairing_complete;
        if (has_saved_wifi) {
            ESP_LOGW(TAG,
                     "Saved Wi-Fi found but MCP pairing is incomplete; reopening setup hotspot without requiring Wi-Fi re-entry");
        }
        ESP_RETURN_ON_ERROR(start_setup_hotspot(), TAG,
                            "Could not start first-time setup");
        xEventGroupWaitBits(s_wifi_events, PORTAL_CONFIGURED_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        /* Give the browser time to receive the success response before its
         * setup Wi-Fi connection is intentionally removed. */
        vTaskDelay(pdMS_TO_TICKS(PORTAL_RESPONSE_GRACE_MS));
        ESP_RETURN_ON_ERROR(stop_setup_hotspot(), TAG,
                            "Could not close first-time setup hotspot");
    }

    ESP_LOGI(TAG, "Connected to Wi-Fi, IP=%s", s_ip_address);
    return ESP_OK;
}

esp_err_t network_get_ip(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(buffer, s_ip_address, buffer_size);
    return ESP_OK;
}

int network_get_rssi(void)
{
    wifi_ap_record_t ap = {0};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

bool network_is_connected(void)
{
    return s_wifi_events &&
           (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}
