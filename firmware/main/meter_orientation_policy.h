#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "meter_rotation.h"

typedef struct {
    uint16_t threshold_mg;
    uint16_t dominance_margin_mg;
    uint16_t stable_ms;
    uint16_t maximum_sample_gap_ms;
    uint16_t ambiguity_grace_ms;
    uint8_t minimum_samples;
} meter_orientation_policy_config_t;

typedef struct {
    meter_orientation_policy_config_t config;
    meter_rotation_t current;
    meter_rotation_t candidate;
    uint32_t candidate_since_ms;
    uint32_t candidate_last_supported_ms;
    uint32_t last_sample_ms;
    uint16_t candidate_samples;
    int32_t filtered_x_mg;
    int32_t filtered_y_mg;
    int32_t filtered_z_mg;
    bool filter_valid;
} meter_orientation_policy_t;

void meter_orientation_policy_init(
    meter_orientation_policy_t *policy,
    meter_rotation_t initial_rotation,
    const meter_orientation_policy_config_t *config);

bool meter_orientation_policy_update(
    meter_orientation_policy_t *policy,
    int32_t x_mg,
    int32_t y_mg,
    int32_t z_mg,
    uint32_t now_ms,
    meter_rotation_t *committed_rotation);
