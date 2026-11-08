#include "meter_battery_policy.h"

#include <stddef.h>

static meter_battery_icon_t icon_for_percent(int percent)
{
    if (percent <= 10) {
        return METER_BATTERY_ICON_EMPTY;
    }
    if (percent <= 25) {
        return METER_BATTERY_ICON_QUARTER;
    }
    if (percent <= 50) {
        return METER_BATTERY_ICON_HALF;
    }
    if (percent <= 75) {
        return METER_BATTERY_ICON_THREE_QUARTERS;
    }
    return METER_BATTERY_ICON_FULL;
}

meter_battery_presentation_t meter_battery_policy_present(
    const meter_battery_snapshot_t *snapshot,
    uint32_t now_ms,
    const meter_battery_policy_config_t *config)
{
    meter_battery_presentation_t result = {
        .state = METER_BATTERY_DISPLAY_UNKNOWN,
        .icon = METER_BATTERY_ICON_EMPTY,
        .percent = -1,
    };
    if (!snapshot || !config || !snapshot->valid) {
        return result;
    }

    const uint32_t age_ms = now_ms - snapshot->sampled_at_ms;
    if (age_ms > config->stale_after_ms) {
        return result;
    }

    if (!snapshot->battery_present) {
        if (snapshot->vbus_present) {
            result.state = METER_BATTERY_DISPLAY_EXTERNAL_POWER;
        }
        return result;
    }

    if (snapshot->percent < 0 || snapshot->percent > 100) {
        return result;
    }

    result.percent = snapshot->percent;
    result.icon = icon_for_percent(snapshot->percent);
    if (snapshot->charging) {
        result.state = METER_BATTERY_DISPLAY_CHARGING;
    } else if (snapshot->vbus_present && snapshot->percent == 100) {
        result.state = METER_BATTERY_DISPLAY_FULL;
    } else if (snapshot->percent <= config->critical_percent) {
        result.state = METER_BATTERY_DISPLAY_CRITICAL;
    } else if (snapshot->percent <= config->low_percent) {
        result.state = METER_BATTERY_DISPLAY_LOW;
    } else {
        result.state = METER_BATTERY_DISPLAY_NORMAL;
    }
    return result;
}
