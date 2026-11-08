#include "meter_discovery.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"

#define METER_MDNS_SERVICE "_codex-meter"
#define METER_MDNS_PROTO "_tcp"
#define METER_MDNS_TIMEOUT_MS 3000
#define METER_MDNS_MAX_RESULTS 8

static const char *TAG = "meter_discovery";
static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_mdns_initialized;
static bool s_has_cached_endpoint;
static meter_endpoint_t s_cached_endpoint;

static bool text_equals(const char *value, size_t value_length, const char *expected)
{
    size_t expected_length = strlen(expected);
    return value && value_length == expected_length &&
           memcmp(value, expected, expected_length) == 0;
}

static bool result_has_txt(const mdns_result_t *result, const char *key, const char *value)
{
    if (!result || !key || !value) {
        return false;
    }

    for (size_t index = 0; index < result->txt_count; ++index) {
        if (result->txt[index].key && strcmp(result->txt[index].key, key) == 0 &&
            text_equals(result->txt[index].value, result->txt_value_len[index], value)) {
            return true;
        }
    }
    return false;
}

static bool result_matches_host(const mdns_result_t *result, const char *host_id)
{
    return result_has_txt(result, "v", "2") && result_has_txt(result, "id", host_id) &&
           result_has_txt(result, "path", "/api/device") &&
           result_has_txt(result, "auth", "hmac-sha256");
}

static bool endpoint_from_result(const mdns_result_t *result, meter_endpoint_t *endpoint)
{
    if (!result || !endpoint || result->port == 0) {
        return false;
    }

    for (const mdns_ip_addr_t *address = result->addr; address; address = address->next) {
        if (address->addr.type != ESP_IPADDR_TYPE_V4) {
            continue;
        }

        int written =
            snprintf(endpoint->host, sizeof(endpoint->host), IPSTR,
                     IP2STR(&address->addr.u_addr.ip4));
        if (written <= 0 || written >= (int)sizeof(endpoint->host)) {
            return false;
        }
        endpoint->port = result->port;
        endpoint->discovered = true;
        return true;
    }
    return false;
}

static bool load_cached_endpoint(meter_endpoint_t *endpoint)
{
    bool found;
    portENTER_CRITICAL(&s_cache_lock);
    found = s_has_cached_endpoint;
    if (found) {
        *endpoint = s_cached_endpoint;
    }
    portEXIT_CRITICAL(&s_cache_lock);
    return found;
}

static void store_cached_endpoint(const meter_endpoint_t *endpoint)
{
    portENTER_CRITICAL(&s_cache_lock);
    s_cached_endpoint = *endpoint;
    s_has_cached_endpoint = true;
    portEXIT_CRITICAL(&s_cache_lock);
}

static esp_err_t resolve_mdns(const meter_config_t *config,
                              meter_endpoint_t *endpoint)
{
    if (config->host_id[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    if (!s_mdns_initialized) {
        esp_err_t error = mdns_init();
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "mDNS initialization failed: %s", esp_err_to_name(error));
            return error;
        }
        s_mdns_initialized = true;
    }

    mdns_result_t *results = NULL;
    esp_err_t error =
        mdns_query_ptr(METER_MDNS_SERVICE, METER_MDNS_PROTO, METER_MDNS_TIMEOUT_MS,
                       METER_MDNS_MAX_RESULTS, &results);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "mDNS query failed: %s", esp_err_to_name(error));
        return error;
    }

    bool found = false;
    for (mdns_result_t *result = results; result; result = result->next) {
        if (result_matches_host(result, config->host_id) &&
            endpoint_from_result(result, endpoint)) {
            found = true;
            break;
        }
    }
    mdns_query_results_free(results);

    if (!found) {
        ESP_LOGW(TAG, "Paired host was not found by mDNS");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Paired host discovered at %s:%u", endpoint->host, endpoint->port);
    return ESP_OK;
}

static esp_err_t resolve_manual(const meter_config_t *config,
                                meter_endpoint_t *endpoint)
{
    if (config->manual_host[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    int written = snprintf(endpoint->host, sizeof(endpoint->host), "%s", config->manual_host);
    if (written <= 0 || written >= (int)sizeof(endpoint->host)) {
        return ESP_ERR_INVALID_SIZE;
    }
    endpoint->port = config->port;
    endpoint->discovered = false;
    ESP_LOGI(TAG, "Using configured host fallback at %s:%u", endpoint->host, endpoint->port);
    return ESP_OK;
}

esp_err_t meter_host_resolve(const meter_config_t *config, meter_endpoint_t *endpoint)
{
    if (!config || !endpoint) {
        return ESP_ERR_INVALID_ARG;
    }
    if (load_cached_endpoint(endpoint)) {
        return ESP_OK;
    }

    memset(endpoint, 0, sizeof(*endpoint));
    esp_err_t error = resolve_mdns(config, endpoint);
    if (error != ESP_OK) {
        error = resolve_manual(config, endpoint);
    }
    if (error == ESP_OK) {
        store_cached_endpoint(endpoint);
    }
    return error;
}

void meter_host_forget(void)
{
    portENTER_CRITICAL(&s_cache_lock);
    memset(&s_cached_endpoint, 0, sizeof(s_cached_endpoint));
    s_has_cached_endpoint = false;
    portEXIT_CRITICAL(&s_cache_lock);
}
