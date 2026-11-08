#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define METER_CONFIG_SCHEMA_VERSION 1

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char host_id[65];
    char pairing_token[65];
    char manual_host[64];
    uint16_t port;
    char setup_psk[16];
} meter_config_t;

/**
 * Loads the runtime configuration from the dedicated "meter" NVS namespace.
 *
 * On first use, legacy CONFIG_METER_* build-time values are migrated. A
 * device-specific setup password is generated and persisted when absent.
 */
esp_err_t meter_config_load(meter_config_t *config);

/**
 * Validates and persists every configuration field. The schema marker is
 * written after all data fields.
 */
esp_err_t meter_config_save(const meter_config_t *config);

/** Erases only the dedicated "meter" NVS namespace. */
esp_err_t meter_config_clear(void);

/**
 * Validates string termination, field lengths, character sets, and port
 * bounds. Empty user-provided fields are valid for an unprovisioned device.
 */
esp_err_t meter_config_validate(const meter_config_t *config);

/** Returns true when the validated configuration is ready for normal use. */
bool meter_config_is_complete(const meter_config_t *config);

#ifdef __cplusplus
}
#endif
