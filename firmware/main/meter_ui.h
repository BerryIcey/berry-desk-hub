#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "meter_battery_model.h"
#include "usage_model.h"

typedef void (*meter_ui_recovery_callback_t)(void *context);
typedef bool (*meter_ui_motion_query_t)(void *context);

void meter_ui_create(lv_display_t *display);
void meter_ui_set_recovery_callback(meter_ui_recovery_callback_t callback,
                                    void *context);
void meter_ui_set_motion_query(meter_ui_motion_query_t query, void *context);
void meter_ui_render(
    const usage_snapshot_t *snapshot,
    const meter_battery_snapshot_t *battery);
