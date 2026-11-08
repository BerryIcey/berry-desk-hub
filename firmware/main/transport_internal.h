#pragma once

#include "esp_err.h"
#include "transport.h"

esp_err_t meter_wifi_transport_start(const meter_transport_callbacks_t *callbacks);
esp_err_t meter_wifi_transport_start_setup(
    const meter_transport_callbacks_t *callbacks);
esp_err_t meter_wifi_transport_request_setup(void);
esp_err_t meter_ble_transport_start(const meter_transport_callbacks_t *callbacks);
