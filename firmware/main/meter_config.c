#include "meter_config.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bootloader_random.h"
#include "esp_random.h"
#include "meter_config_requirements.h"
#include "nvs.h"
#include "sdkconfig.h"

#define METER_NVS_NAMESPACE "meter"
#define METER_NVS_KEY_SCHEMA "schema"
#define METER_NVS_KEY_WIFI_SSID "wifi_ssid"
#define METER_NVS_KEY_WIFI_PASSWORD "wifi_pass"
#define METER_NVS_KEY_HOST_ID "host_id"
#define METER_NVS_KEY_PAIRING_TOKEN "pair_token"
#define METER_NVS_KEY_MANUAL_HOST "manual_host"
#define METER_NVS_KEY_PORT "port"
#define METER_NVS_KEY_SETUP_PSK "setup_psk"

#define METER_DEFAULT_PORT 8787
#define METER_HOST_ID_MIN_LENGTH 8
#define METER_PAIRING_TOKEN_MIN_LENGTH 32
#define METER_SETUP_PSK_MIN_LENGTH 10
#define METER_SETUP_PSK_LENGTH 12

static const char s_setup_alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

_Static_assert(sizeof(s_setup_alphabet) - 1 == 32,
               "setup password alphabet must contain exactly 32 characters");

static bool bounded_length(const char *value, size_t capacity, size_t *length)
{
    const char *terminator = memchr(value, '\0', capacity);
    if (!terminator) {
        return false;
    }

    *length = (size_t)(terminator - value);
    return true;
}

static bool has_only_base64url_characters(const char *value, size_t length)
{
    for (size_t index = 0; index < length; ++index) {
        unsigned char character = (unsigned char)value[index];
        if (!isalnum(character) && character != '-' && character != '_') {
            return false;
        }
    }
    return true;
}

static bool is_valid_setup_psk(const char *value, size_t capacity)
{
    size_t length = 0;
    if (!bounded_length(value, capacity, &length) ||
        length < METER_SETUP_PSK_MIN_LENGTH || length > METER_SETUP_PSK_LENGTH) {
        return false;
    }

    for (size_t index = 0; index < length; ++index) {
        if (!strchr(s_setup_alphabet, value[index])) {
            return false;
        }
    }
    return true;
}

static bool is_valid_manual_host(const char *value, size_t length)
{
    if (length == 0) {
        return true;
    }

    if (!isalnum((unsigned char)value[0]) ||
        !isalnum((unsigned char)value[length - 1])) {
        return false;
    }

    size_t label_length = 0;
    for (size_t index = 0; index < length; ++index) {
        unsigned char character = (unsigned char)value[index];

        if (isalnum(character)) {
            ++label_length;
            continue;
        }

        if (character == '-') {
            if (label_length == 0) {
                return false;
            }
            ++label_length;
            continue;
        }

        if (character == '.') {
            if (label_length == 0 || label_length > 63 || value[index - 1] == '-') {
                return false;
            }
            label_length = 0;
            continue;
        }

        /*
         * This intentionally rejects schemes, paths, query strings, embedded
         * ports, credentials, whitespace, and IPv6 literals. The HTTP port is
         * stored separately, and the current transport accepts hostnames or
         * IPv4 addresses.
         */
        return false;
    }

    return label_length > 0 && label_length <= 63;
}

static bool has_valid_wifi_password(const char *value, size_t length)
{
    if (length == 0) {
        return true;
    }

    if (length == 64) {
        for (size_t index = 0; index < length; ++index) {
            if (!isxdigit((unsigned char)value[index])) {
                return false;
            }
        }
        return true;
    }

    if (length < 8 || length > 63) {
        return false;
    }

    for (size_t index = 0; index < length; ++index) {
        unsigned char character = (unsigned char)value[index];
        if (character < 0x20 || character > 0x7e) {
            return false;
        }
    }
    return true;
}

static esp_err_t copy_build_value(char *target, size_t capacity, const char *value)
{
    size_t length = strlen(value);
    if (length >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(target, value, length + 1);
    return ESP_OK;
}

static uint16_t configured_default_port(void)
{
#ifdef CONFIG_METER_PORT
    if (CONFIG_METER_PORT > 0 && CONFIG_METER_PORT <= UINT16_MAX) {
        return (uint16_t)CONFIG_METER_PORT;
    }
#endif
    return METER_DEFAULT_PORT;
}

static void generate_setup_psk(char output[16])
{
    uint8_t random_bytes[METER_SETUP_PSK_LENGTH];
    bootloader_random_enable();
    esp_fill_random(random_bytes, sizeof(random_bytes));
    bootloader_random_disable();

    for (size_t index = 0; index < sizeof(random_bytes); ++index) {
        output[index] = s_setup_alphabet[random_bytes[index] & 0x1f];
    }
    output[sizeof(random_bytes)] = '\0';
}

static esp_err_t migrate_build_configuration(meter_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->port = configured_default_port();
    esp_err_t error = ESP_OK;

#ifdef CONFIG_METER_WIFI_SSID
    error =
        copy_build_value(config->wifi_ssid, sizeof(config->wifi_ssid), CONFIG_METER_WIFI_SSID);
    if (error != ESP_OK) {
        return error;
    }
#endif

#ifdef CONFIG_METER_WIFI_PASSWORD
    error = copy_build_value(config->wifi_password, sizeof(config->wifi_password),
                             CONFIG_METER_WIFI_PASSWORD);
    if (error != ESP_OK) {
        return error;
    }
#endif

#ifdef CONFIG_METER_HOST
    error =
        copy_build_value(config->manual_host, sizeof(config->manual_host), CONFIG_METER_HOST);
    if (error != ESP_OK) {
        return error;
    }
#endif

#ifdef CONFIG_METER_HOST_ID
    error = copy_build_value(config->host_id, sizeof(config->host_id), CONFIG_METER_HOST_ID);
    if (error != ESP_OK) {
        return error;
    }
#endif

#ifdef CONFIG_METER_PAIRING_TOKEN
    error = copy_build_value(config->pairing_token, sizeof(config->pairing_token),
                             CONFIG_METER_PAIRING_TOKEN);
    if (error != ESP_OK) {
        return error;
    }
#endif

    generate_setup_psk(config->setup_psk);
    return meter_config_validate(config);
}

static esp_err_t load_string(nvs_handle_t handle, const char *key, char *target,
                             size_t capacity, bool *needs_save)
{
    size_t required = capacity;
    esp_err_t error = nvs_get_str(handle, key, target, &required);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        target[0] = '\0';
        *needs_save = true;
        return ESP_OK;
    }
    if (error == ESP_ERR_NVS_INVALID_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    return error;
}

esp_err_t meter_config_validate(const meter_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_length = 0;
    size_t password_length = 0;
    size_t host_id_length = 0;
    size_t pairing_token_length = 0;
    size_t manual_host_length = 0;

    if (!bounded_length(config->wifi_ssid, sizeof(config->wifi_ssid), &ssid_length) ||
        !bounded_length(config->wifi_password, sizeof(config->wifi_password),
                        &password_length) ||
        !bounded_length(config->host_id, sizeof(config->host_id), &host_id_length) ||
        !bounded_length(config->pairing_token, sizeof(config->pairing_token),
                        &pairing_token_length) ||
        !bounded_length(config->manual_host, sizeof(config->manual_host),
                        &manual_host_length)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (ssid_length > 32 || password_length > 64 || host_id_length > 64 ||
        pairing_token_length > 64 || manual_host_length > 63) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t index = 0; index < ssid_length; ++index) {
        unsigned char character = (unsigned char)config->wifi_ssid[index];
        if (character < 0x20 || character == 0x7f) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (!has_valid_wifi_password(config->wifi_password, password_length)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (host_id_length > 0 &&
        (host_id_length < METER_HOST_ID_MIN_LENGTH ||
         !has_only_base64url_characters(config->host_id, host_id_length))) {
        return ESP_ERR_INVALID_ARG;
    }

    if (pairing_token_length > 0 &&
        (pairing_token_length < METER_PAIRING_TOKEN_MIN_LENGTH ||
         !has_only_base64url_characters(config->pairing_token, pairing_token_length))) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!is_valid_manual_host(config->manual_host, manual_host_length) ||
        config->port == 0 || !is_valid_setup_psk(config->setup_psk,
                                                 sizeof(config->setup_psk))) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

bool meter_config_is_complete(const meter_config_t *config)
{
    if (meter_config_validate(config) != ESP_OK) {
        return false;
    }

    return meter_config_required_fields_present(
        config->wifi_ssid, config->pairing_token, config->host_id,
        config->manual_host,
#if CONFIG_METER_TRANSPORT_BLE
        true
#else
        false
#endif
    );
}

esp_err_t meter_config_save(const meter_config_t *config)
{
    esp_err_t error = meter_config_validate(config);
    if (error != ESP_OK) {
        return error;
    }

    nvs_handle_t handle;
    error = nvs_open(METER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }

    /*
     * Invalidate the previous record before updating individual keys. If
     * power is lost during the second transaction, load will never accept a
     * mixture of old and new fields as a complete schema-v1 record.
     */
    error = nvs_erase_key(handle, METER_NVS_KEY_SCHEMA);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    if (error != ESP_OK) {
        nvs_close(handle);
        return error;
    }

    error = nvs_set_str(handle, METER_NVS_KEY_WIFI_SSID, config->wifi_ssid);
    if (error == ESP_OK) {
        error = nvs_set_str(handle, METER_NVS_KEY_WIFI_PASSWORD, config->wifi_password);
    }
    if (error == ESP_OK) {
        error = nvs_set_str(handle, METER_NVS_KEY_HOST_ID, config->host_id);
    }
    if (error == ESP_OK) {
        error = nvs_set_str(handle, METER_NVS_KEY_PAIRING_TOKEN, config->pairing_token);
    }
    if (error == ESP_OK) {
        error = nvs_set_str(handle, METER_NVS_KEY_MANUAL_HOST, config->manual_host);
    }
    if (error == ESP_OK) {
        error = nvs_set_u16(handle, METER_NVS_KEY_PORT, config->port);
    }
    if (error == ESP_OK) {
        error = nvs_set_str(handle, METER_NVS_KEY_SETUP_PSK, config->setup_psk);
    }
    if (error == ESP_OK) {
        error = nvs_set_u16(handle, METER_NVS_KEY_SCHEMA, METER_CONFIG_SCHEMA_VERSION);
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }

    nvs_close(handle);
    return error;
}

esp_err_t meter_config_load(meter_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));

    nvs_handle_t handle;
    esp_err_t error = nvs_open(METER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }

    uint16_t schema = 0;
    error = nvs_get_u16(handle, METER_NVS_KEY_SCHEMA, &schema);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);

        error = migrate_build_configuration(config);
        if (error != ESP_OK) {
            return error;
        }
        return meter_config_save(config);
    }
    if (error != ESP_OK) {
        nvs_close(handle);
        return error;
    }
    if (schema != METER_CONFIG_SCHEMA_VERSION) {
        /*
         * Preserve a future schema instead of erasing it automatically. Keep
         * only the setup key (when valid) so an explicit portal submission can
         * deliberately replace the record with this firmware's schema.
         */
        config->port = configured_default_port();
        size_t setup_psk_size = sizeof(config->setup_psk);
        esp_err_t setup_error =
            nvs_get_str(handle, METER_NVS_KEY_SETUP_PSK, config->setup_psk,
                        &setup_psk_size);
        if (setup_error != ESP_OK ||
            !is_valid_setup_psk(config->setup_psk, sizeof(config->setup_psk))) {
            memset(config->setup_psk, 0, sizeof(config->setup_psk));
            generate_setup_psk(config->setup_psk);
        }
        nvs_close(handle);
        return ESP_ERR_NOT_SUPPORTED;
    }

    bool needs_save = false;
    error = load_string(handle, METER_NVS_KEY_WIFI_SSID, config->wifi_ssid,
                        sizeof(config->wifi_ssid), &needs_save);
    if (error == ESP_OK) {
        error = load_string(handle, METER_NVS_KEY_WIFI_PASSWORD, config->wifi_password,
                            sizeof(config->wifi_password), &needs_save);
    }
    if (error == ESP_OK) {
        error = load_string(handle, METER_NVS_KEY_HOST_ID, config->host_id,
                            sizeof(config->host_id), &needs_save);
    }
    if (error == ESP_OK) {
        error = load_string(handle, METER_NVS_KEY_PAIRING_TOKEN, config->pairing_token,
                            sizeof(config->pairing_token), &needs_save);
    }
    if (error == ESP_OK) {
        error = load_string(handle, METER_NVS_KEY_MANUAL_HOST, config->manual_host,
                            sizeof(config->manual_host), &needs_save);
    }
    if (error == ESP_OK) {
        error = nvs_get_u16(handle, METER_NVS_KEY_PORT, &config->port);
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            config->port = configured_default_port();
            needs_save = true;
            error = ESP_OK;
        }
    }
    if (error == ESP_OK) {
        error = load_string(handle, METER_NVS_KEY_SETUP_PSK, config->setup_psk,
                            sizeof(config->setup_psk), &needs_save);
    }

    nvs_close(handle);
    if (error != ESP_OK) {
        return error;
    }

    if (!is_valid_setup_psk(config->setup_psk, sizeof(config->setup_psk))) {
        generate_setup_psk(config->setup_psk);
        needs_save = true;
    }

    error = meter_config_validate(config);
    if (error != ESP_OK) {
        return error;
    }

    return needs_save ? meter_config_save(config) : ESP_OK;
}

esp_err_t meter_config_clear(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(METER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }

    error = nvs_erase_all(handle);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }

    nvs_close(handle);
    return error;
}
