#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool valid;
    bool battery_present;
    bool charging;
    bool vbus_present;
    int percent;
    uint16_t voltage_mv;
    uint32_t sampled_at_ms;
} meter_battery_snapshot_t;
