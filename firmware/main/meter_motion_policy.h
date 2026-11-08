#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t threshold_mg;
    uint16_t maximum_sample_gap_ms;
    uint16_t active_hold_ms;
} meter_motion_policy_config_t;

typedef struct {
    meter_motion_policy_config_t config;
    int32_t previous_x_mg;
    int32_t previous_y_mg;
    int32_t previous_z_mg;
    int32_t baseline_x_mg;
    int32_t baseline_y_mg;
    int32_t baseline_z_mg;
    /* Q5 accumulators retain 1/32 mg EMA precision at low thresholds. */
    int32_t baseline_x_q5;
    int32_t baseline_y_q5;
    int32_t baseline_z_q5;
    uint32_t last_sample_ms;
    uint32_t last_detected_ms;
    bool sample_valid;
    bool active;
} meter_motion_policy_t;

void meter_motion_policy_init(
    meter_motion_policy_t *policy,
    const meter_motion_policy_config_t *config);

/*
 * Returns true for every valid sample while activity is held active. The
 * caller can therefore publish a heartbeat instead of only a rising edge.
 */
bool meter_motion_policy_update(
    meter_motion_policy_t *policy,
    int32_t x_mg,
    int32_t y_mg,
    int32_t z_mg,
    uint32_t now_ms);
