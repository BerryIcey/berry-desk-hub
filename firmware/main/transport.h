#pragma once

#include "esp_err.h"
#include "usage_model.h"

typedef void (*meter_snapshot_callback_t)(const usage_snapshot_t *snapshot, void *context);
typedef void (*meter_link_callback_t)(meter_link_state_t state, void *context);
typedef void (*meter_setup_callback_t)(const char *ssid, const char *password, void *context);

typedef struct {
    meter_snapshot_callback_t on_snapshot;
    meter_link_callback_t on_link;
    meter_setup_callback_t on_setup;
    void *context;
} meter_transport_callbacks_t;

esp_err_t meter_transport_start(const meter_transport_callbacks_t *callbacks);

/** Opens the local protected setup portal after an explicit physical action. */
esp_err_t meter_transport_request_setup(void);
