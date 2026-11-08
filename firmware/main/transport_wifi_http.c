#include "transport_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "meter_config.h"
#include "meter_discovery.h"
#include "meter_provisioning.h"
#include "usage_payload.h"

#define WIFI_CONNECTED_BIT BIT0
#define HTTP_BUFFER_SIZE 4096
#define HOST_FAILURES_BEFORE_REDISCOVERY 3
#define HMAC_NONCE_BYTES 16
#define SHA256_HEX_LENGTH 64
#define METER_AUTH_DOMAIN "codex-meter-v2"

static const char *TAG = "meter_wifi";

typedef struct {
    char data[HTTP_BUFFER_SIZE];
    char response_signature[SHA256_HEX_LENGTH + 1];
    size_t length;
    bool overflow;
    bool has_response_signature;
} http_buffer_t;

typedef enum {
    FETCH_RESULT_OK = 0,
    FETCH_RESULT_RETRY,
    FETCH_RESULT_AUTH,
} fetch_result_t;

static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_provisioning_mutex;
static TaskHandle_t s_recovery_task;
static meter_transport_callbacks_t s_callbacks;
static meter_config_t s_config;
static bool s_has_snapshot;
static int64_t s_last_captured_at;
static TickType_t s_last_freshness_tick;
static volatile uint32_t s_disconnect_count;
static bool s_initialized;
static bool s_station_started;

static void publish_link(meter_link_state_t state)
{
    if (s_callbacks.on_link) {
        s_callbacks.on_link(state, s_callbacks.context);
    }
}

static void provisioning_ready(const char *ssid, const char *password, void *context)
{
    (void)context;
    publish_link(METER_LINK_SETUP);
    if (s_callbacks.on_setup) {
        s_callbacks.on_setup(ssid, password, s_callbacks.context);
    }
}

static esp_err_t start_provisioning(bool keep_station)
{
    xSemaphoreTake(s_provisioning_mutex, portMAX_DELAY);
    esp_err_t error =
        meter_provisioning_start(&s_config, keep_station, provisioning_ready, NULL);
    xSemaphoreGive(s_provisioning_mutex);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Setup portal failed: %s", esp_err_to_name(error));
    }
    return error;
}

static esp_err_t stop_provisioning(void)
{
    xSemaphoreTake(s_provisioning_mutex, portMAX_DELAY);
    esp_err_t error = meter_provisioning_stop();
    xSemaphoreGive(s_provisioning_mutex);
    return error;
}

static bool is_lower_hex_string(const char *value, size_t length)
{
    if (!value) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        if (!((value[index] >= '0' && value[index] <= '9') ||
              (value[index] >= 'a' && value[index] <= 'f'))) {
            return false;
        }
    }
    return value[length] == '\0';
}

static void bytes_to_hex(const uint8_t *bytes, size_t length, char *output)
{
    static const char alphabet[] = "0123456789abcdef";
    for (size_t index = 0; index < length; ++index) {
        output[index * 2] = alphabet[bytes[index] >> 4];
        output[index * 2 + 1] = alphabet[bytes[index] & 0x0f];
    }
    output[length * 2] = '\0';
}

static bool digest_sha256_hex(const void *data, size_t length,
                              char output[SHA256_HEX_LENGTH + 1])
{
    uint8_t digest[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || mbedtls_md(info, data, length, digest) != 0) {
        return false;
    }
    bytes_to_hex(digest, sizeof(digest), output);
    memset(digest, 0, sizeof(digest));
    return true;
}

static bool hmac_sha256_hex(const char *key, const char *message,
                            char output[SHA256_HEX_LENGTH + 1])
{
    uint8_t digest[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info ||
        mbedtls_md_hmac(info, (const unsigned char *)key, strlen(key),
                        (const unsigned char *)message, strlen(message), digest) != 0) {
        return false;
    }
    bytes_to_hex(digest, sizeof(digest), output);
    memset(digest, 0, sizeof(digest));
    return true;
}

static bool constant_time_equal(const char *left, const char *right, size_t length)
{
    unsigned difference = 0;
    for (size_t index = 0; index < length; ++index) {
        difference |= (unsigned char)left[index] ^ (unsigned char)right[index];
    }
    return difference == 0;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_buffer_t *buffer = (http_buffer_t *)event->user_data;
    if (!buffer) {
        return ESP_OK;
    }

    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
        event->header_value &&
        strcasecmp(event->header_key, "X-Meter-Signature") == 0) {
        size_t length = strnlen(event->header_value, SHA256_HEX_LENGTH + 2);
        if (length == SHA256_HEX_LENGTH &&
            is_lower_hex_string(event->header_value, SHA256_HEX_LENGTH)) {
            memcpy(buffer->response_signature, event->header_value,
                   SHA256_HEX_LENGTH + 1);
            buffer->has_response_signature = true;
        }
        return ESP_OK;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    size_t available = sizeof(buffer->data) - 1 - buffer->length;
    size_t copy_length = (size_t)event->data_len;
    if (copy_length > available) {
        copy_length = available;
        buffer->overflow = true;
    }
    if (copy_length > 0) {
        memcpy(buffer->data + buffer->length, event->data, copy_length);
        buffer->length += copy_length;
        buffer->data[buffer->length] = '\0';
    }
    return ESP_OK;
}

static fetch_result_t fetch_usage(const meter_endpoint_t *endpoint, usage_snapshot_t *result)
{
    char url[192];
    int written = snprintf(url, sizeof(url), "http://%s:%u/api/device", endpoint->host,
                           endpoint->port);
    if (written < 0 || written >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "Host URL is too long");
        return FETCH_RESULT_RETRY;
    }

    uint8_t nonce_bytes[HMAC_NONCE_BYTES];
    char nonce[HMAC_NONCE_BYTES * 2 + 1];
    esp_fill_random(nonce_bytes, sizeof(nonce_bytes));
    bytes_to_hex(nonce_bytes, sizeof(nonce_bytes), nonce);
    memset(nonce_bytes, 0, sizeof(nonce_bytes));

    char request_canonical[128];
    written = snprintf(request_canonical, sizeof(request_canonical),
                       METER_AUTH_DOMAIN "\nrequest\n%s\nGET\n/api/device", nonce);
    if (written < 0 || written >= (int)sizeof(request_canonical)) {
        return FETCH_RESULT_AUTH;
    }

    char request_signature[SHA256_HEX_LENGTH + 1];
    if (!hmac_sha256_hex(s_config.pairing_token, request_canonical,
                         request_signature)) {
        return FETCH_RESULT_AUTH;
    }
    memset(request_canonical, 0, sizeof(request_canonical));

    char authorization[sizeof("Meter-HMAC ") + SHA256_HEX_LENGTH];
    written = snprintf(authorization, sizeof(authorization), "Meter-HMAC %s",
                       request_signature);
    memset(request_signature, 0, sizeof(request_signature));
    if (written < 0 || written >= (int)sizeof(authorization)) {
        ESP_LOGE(TAG, "Pairing token is invalid");
        return FETCH_RESULT_AUTH;
    }

    http_buffer_t buffer = {0};
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &buffer,
        .timeout_ms = 8000,
        .buffer_size = 1024,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return FETCH_RESULT_RETRY;
    }

    esp_err_t error = esp_http_client_set_header(client, "Accept", "application/json");
    if (error == ESP_OK) {
        error = esp_http_client_set_header(client, "Authorization", authorization);
    }
    if (error == ESP_OK) {
        error = esp_http_client_set_header(client, "X-Meter-Nonce", nonce);
    }
    if (error == ESP_OK) {
        error = esp_http_client_set_header(client, "Connection", "close");
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Unable to prepare authenticated request: %s",
                 esp_err_to_name(error));
        esp_http_client_cleanup(client);
        memset(authorization, 0, sizeof(authorization));
        return FETCH_RESULT_RETRY;
    }

    error = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    memset(authorization, 0, sizeof(authorization));

    if (error != ESP_OK || buffer.overflow || buffer.length == 0 ||
        !buffer.has_response_signature) {
        ESP_LOGW(TAG, "GET failed or unsigned: %s, HTTP %d, bytes %u",
                 esp_err_to_name(error), status_code, (unsigned)buffer.length);
        return FETCH_RESULT_RETRY;
    }

    char body_hash[SHA256_HEX_LENGTH + 1];
    char response_canonical[192];
    char expected_signature[SHA256_HEX_LENGTH + 1];
    if (!digest_sha256_hex(buffer.data, buffer.length, body_hash)) {
        return FETCH_RESULT_RETRY;
    }
    written = snprintf(response_canonical, sizeof(response_canonical),
                       METER_AUTH_DOMAIN "\nresponse\n%s\n%d\n%s", nonce,
                       status_code, body_hash);
    memset(body_hash, 0, sizeof(body_hash));
    if (written < 0 || written >= (int)sizeof(response_canonical) ||
        !hmac_sha256_hex(s_config.pairing_token, response_canonical,
                         expected_signature)) {
        memset(response_canonical, 0, sizeof(response_canonical));
        return FETCH_RESULT_RETRY;
    }
    memset(response_canonical, 0, sizeof(response_canonical));
    bool signature_matches =
        constant_time_equal(buffer.response_signature, expected_signature,
                            SHA256_HEX_LENGTH);
    memset(expected_signature, 0, sizeof(expected_signature));
    memset(buffer.response_signature, 0, sizeof(buffer.response_signature));
    if (!signature_matches) {
        ESP_LOGW(TAG, "Host response signature is invalid");
        return FETCH_RESULT_RETRY;
    }

    if (status_code == 401 || status_code == 403) {
        ESP_LOGW(TAG, "Host rejected the pairing token");
        return FETCH_RESULT_AUTH;
    }
    if (status_code != 200) {
        ESP_LOGW(TAG, "Host returned HTTP %d", status_code);
        return FETCH_RESULT_RETRY;
    }

    if (!meter_usage_payload_parse(buffer.data, result)) {
        ESP_LOGW(TAG, "Invalid usage JSON");
        return FETCH_RESULT_RETRY;
    }
    return FETCH_RESULT_OK;
}

static void usage_task(void *argument)
{
    (void)argument;
    unsigned endpoint_failures = 0;
    unsigned host_failures = 0;

    for (;;) {
        xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        publish_link(s_has_snapshot ? METER_LINK_STALE : METER_LINK_DISCOVERING);
        meter_endpoint_t endpoint;
        esp_err_t resolve_error = meter_host_resolve(&s_config, &endpoint);
        if (resolve_error != ESP_OK) {
            if (host_failures < UINT32_MAX) {
                ++host_failures;
            }
            if (host_failures >= CONFIG_METER_HOST_RETRY_LIMIT) {
                ESP_LOGW(TAG, "Host recovery portal enabled after repeated discovery failures");
                start_provisioning(true);
            }
            publish_link(s_has_snapshot ? METER_LINK_STALE : METER_LINK_CONNECTING);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_METER_REFRESH_SECONDS * 1000));
            continue;
        }

        usage_snapshot_t latest;
        fetch_result_t fetched = fetch_usage(&endpoint, &latest);
        if ((xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) == 0) {
            publish_link(METER_LINK_OFFLINE);
        } else if (fetched == FETCH_RESULT_OK) {
            endpoint_failures = 0;
            host_failures = 0;
            esp_err_t stop_error = stop_provisioning();
            if (stop_error != ESP_OK) {
                ESP_LOGW(TAG, "Unable to stop setup portal: %s",
                         esp_err_to_name(stop_error));
            }
            if (latest.captured_at > 0 && s_has_snapshot &&
                latest.captured_at == s_last_captured_at) {
                latest.freshness_tick = s_last_freshness_tick;
            } else {
                s_last_captured_at = latest.captured_at;
                s_last_freshness_tick = latest.freshness_tick;
            }
            s_has_snapshot = true;
            if (s_callbacks.on_snapshot) {
                s_callbacks.on_snapshot(&latest, s_callbacks.context);
            }
            ESP_LOGI(TAG, "Usage updated: used=%d remaining=%d; host refresh in %ds",
                     latest.used, latest.remaining, latest.next_poll_in);
        } else if (fetched == FETCH_RESULT_AUTH) {
            publish_link(METER_LINK_AUTH_REQUIRED);
            start_provisioning(true);
        } else {
            if (host_failures < UINT32_MAX) {
                ++host_failures;
            }
            if (++endpoint_failures >= HOST_FAILURES_BEFORE_REDISCOVERY) {
                endpoint_failures = 0;
                meter_host_forget();
            }
            if (host_failures >= CONFIG_METER_HOST_RETRY_LIMIT) {
                ESP_LOGW(TAG, "Host recovery portal enabled after repeated request failures");
                start_provisioning(true);
            }
            publish_link(s_has_snapshot ? METER_LINK_STALE : METER_LINK_CONNECTING);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_METER_REFRESH_SECONDS * 1000));
    }
}

static void recovery_task(void *argument)
{
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_disconnect_count >= CONFIG_METER_WIFI_RETRY_LIMIT) {
            ESP_LOGW(TAG, "Wi-Fi recovery portal enabled after repeated disconnects");
            start_provisioning(true);
        }
    }
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    (void)argument;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        publish_link(METER_LINK_CONNECTING);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        meter_host_forget();
        if (s_disconnect_count < UINT32_MAX) {
            ++s_disconnect_count;
        }
        publish_link(METER_LINK_OFFLINE);
        if (s_recovery_task &&
            s_disconnect_count >= CONFIG_METER_WIFI_RETRY_LIMIT) {
            xTaskNotifyGive(s_recovery_task);
        }
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_disconnect_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        publish_link(METER_LINK_DISCOVERING);
    }
}

static esp_err_t wifi_initialize(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop init failed");
    if (!esp_netif_create_default_wifi_sta()) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "Wi-Fi storage config failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG,
        "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL), TAG,
        "IP event registration failed");
    return ESP_OK;
}

static esp_err_t wifi_station_start(void)
{
    wifi_config_t wifi_config = {0};
    size_t ssid_length = strlen(s_config.wifi_ssid);
    size_t password_length = strlen(s_config.wifi_password);
    memcpy(wifi_config.sta.ssid, s_config.wifi_ssid, ssid_length);
    memcpy(wifi_config.sta.password, s_config.wifi_password, password_length);
    wifi_config.sta.threshold.authmode =
        password_length > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG,
                        "Wi-Fi config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Wi-Fi power-save config failed");
    return ESP_OK;
}

static esp_err_t load_configuration(void)
{
    esp_err_t error = meter_config_load(&s_config);
    if (error == ESP_OK || error == ESP_ERR_NOT_SUPPORTED) {
        return error;
    }

    ESP_LOGW(TAG, "Runtime configuration is invalid; reopening first-run setup");
    error = meter_config_clear();
    if (error != ESP_OK) {
        return error;
    }
    return meter_config_load(&s_config);
}

static esp_err_t initialize_transport(
    const meter_transport_callbacks_t *callbacks,
    bool *schema_recovery)
{
    if (!callbacks || !callbacks->on_snapshot || !callbacks->on_link ||
        !schema_recovery) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized || s_wifi_events || s_provisioning_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    s_callbacks = *callbacks;
    s_wifi_events = xEventGroupCreate();
    s_provisioning_mutex = xSemaphoreCreateMutex();
    if (!s_wifi_events || !s_provisioning_mutex) {
        if (s_wifi_events) {
            vEventGroupDelete(s_wifi_events);
            s_wifi_events = NULL;
        }
        if (s_provisioning_mutex) {
            vSemaphoreDelete(s_provisioning_mutex);
            s_provisioning_mutex = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = load_configuration();
    *schema_recovery = result == ESP_ERR_NOT_SUPPORTED;
    if (result != ESP_OK && !*schema_recovery) {
        ESP_LOGE(TAG, "Configuration load failed: %s", esp_err_to_name(result));
        publish_link(METER_LINK_SETUP);
        return result;
    }

    result = wifi_initialize();
    if (result != ESP_OK) {
        return result;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t meter_wifi_transport_start(const meter_transport_callbacks_t *callbacks)
{
    bool schema_recovery = false;
    esp_err_t result = initialize_transport(callbacks, &schema_recovery);
    if (result != ESP_OK) {
        return result;
    }

    if (schema_recovery || !meter_config_is_complete(&s_config)) {
        if (schema_recovery) {
            ESP_LOGW(TAG, "Newer configuration schema found; starting explicit recovery portal");
        }
        ESP_LOGI(TAG, "Runtime configuration is incomplete; starting setup portal");
        return start_provisioning(false);
    }

    BaseType_t created =
        xTaskCreate(recovery_task, "wifi_recovery", 4096, NULL, 4, &s_recovery_task);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    result = wifi_station_start();
    if (result != ESP_OK) {
        vTaskDelete(s_recovery_task);
        s_recovery_task = NULL;
        return result;
    }
    s_station_started = true;

    created = xTaskCreate(usage_task, "usage_fetch", 8192, NULL, 5, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t meter_wifi_transport_start_setup(
    const meter_transport_callbacks_t *callbacks)
{
    bool schema_recovery = false;
    esp_err_t result = initialize_transport(callbacks, &schema_recovery);
    if (result == ESP_ERR_INVALID_STATE && s_initialized) {
        /*
         * A previous start may have initialized Wi-Fi before the portal or
         * worker task failed. Keep the physical recovery action retryable
         * instead of permanently stranding a BLE-only device.
         */
        return meter_wifi_transport_request_setup();
    }
    if (result != ESP_OK) {
        return result;
    }
    if (schema_recovery) {
        ESP_LOGW(TAG, "Newer configuration schema found; starting explicit recovery portal");
    }
    return start_provisioning(false);
}

esp_err_t meter_wifi_transport_request_setup(void)
{
    if (!s_initialized || !s_provisioning_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    return start_provisioning(s_station_started);
}
