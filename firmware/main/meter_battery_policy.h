#pragma once

#include <stdint.h>

#include "meter_battery_model.h"

typedef enum {
    METER_BATTERY_DISPLAY_UNKNOWN = 0,
    METER_BATTERY_DISPLAY_EXTERNAL_POWER,
    METER_BATTERY_DISPLAY_NORMAL,
    METER_BATTERY_DISPLAY_LOW,
    METER_BATTERY_DISPLAY_CRITICAL,
    METER_BATTERY_DISPLAY_CHARGING,
    METER_BATTERY_DISPLAY_FULL,
} meter_battery_display_state_t;

typedef enum {
    METER_BATTERY_ICON_EMPTY = 0,
    METER_BATTERY_ICON_QUARTER,
    METER_BATTERY_ICON_HALF,
    METER_BATTERY_ICON_THREE_QUARTERS,
    METER_BATTERY_ICON_FULL,
} meter_battery_icon_t;

typedef struct {
    int critical_percent;
    int low_percent;
    uint32_t stale_after_ms;
} meter_battery_policy_config_t;

typedef struct {
    meter_battery_display_state_t state;
    meter_battery_icon_t icon;
    int percent;
} meter_battery_presentation_t;

meter_battery_presentation_t meter_battery_policy_present(
    const meter_battery_snapshot_t *snapshot,
    uint32_t now_ms,
    const meter_battery_policy_config_t *config);
