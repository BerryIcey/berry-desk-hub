#pragma once

#include "esp_err.h"

#include "meter_battery_model.h"

typedef void (*meter_battery_update_cb_t)(
    const meter_battery_snapshot_t *snapshot,
    void *context);

esp_err_t meter_battery_start(meter_battery_update_cb_t callback, void *context);
