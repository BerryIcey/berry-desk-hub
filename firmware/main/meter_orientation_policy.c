#include "meter_orientation_policy.h"

#include <stddef.h>

#define METER_GRAVITY_MIN_MG 650
#define METER_GRAVITY_MAX_MG 1350
#define METER_ACCEL_PLAUSIBLE_MAX_MG 4000
#define METER_FILTER_OLD_WEIGHT 3
#define METER_FILTER_TOTAL_WEIGHT 4

static int64_t absolute_value(int32_t value)
{
    return value < 0 ? -(int64_t)value : value;
}

static void reset_candidate(meter_orientation_policy_t *policy)
{
    policy->candidate = METER_ROTATION_INVALID;
    policy->candidate_since_ms = 0;
    policy->candidate_last_supported_ms = 0;
    policy->candidate_samples = 0;
}

static bool sample_is_gravity(int32_t x_mg, int32_t y_mg, int32_t z_mg)
{
    if (absolute_value(x_mg) > METER_ACCEL_PLAUSIBLE_MAX_MG ||
        absolute_value(y_mg) > METER_ACCEL_PLAUSIBLE_MAX_MG ||
        absolute_value(z_mg) > METER_ACCEL_PLAUSIBLE_MAX_MG) {
        return false;
    }

    const int64_t magnitude_squared =
        (int64_t)x_mg * x_mg + (int64_t)y_mg * y_mg + (int64_t)z_mg * z_mg;
    const int64_t minimum_squared =
        (int64_t)METER_GRAVITY_MIN_MG * METER_GRAVITY_MIN_MG;
    const int64_t maximum_squared =
        (int64_t)METER_GRAVITY_MAX_MG * METER_GRAVITY_MAX_MG;
    return magnitude_squared >= minimum_squared && magnitude_squared <= maximum_squared;
}

static meter_rotation_t classify_rotation(
    const meter_orientation_policy_t *policy,
    bool *ambiguous)
{
    const int64_t absolute_x = absolute_value(policy->filtered_x_mg);
    const int64_t absolute_y = absolute_value(policy->filtered_y_mg);
    const int64_t dominant = absolute_x > absolute_y ? absolute_x : absolute_y;
    const int64_t secondary = absolute_x > absolute_y ? absolute_y : absolute_x;

    *ambiguous = false;
    if (dominant < policy->config.threshold_mg) {
        return METER_ROTATION_INVALID;
    }
    if (dominant - secondary < policy->config.dominance_margin_mg) {
        *ambiguous = true;
        return METER_ROTATION_INVALID;
    }

    meter_rotation_t detected;
    if (absolute_x > absolute_y) {
        detected =
            policy->filtered_x_mg > 0 ? METER_ROTATION_270 : METER_ROTATION_90;
    } else {
        detected =
            policy->filtered_y_mg > 0 ? METER_ROTATION_0 : METER_ROTATION_180;
    }
    return detected;
}

void meter_orientation_policy_init(
    meter_orientation_policy_t *policy,
    meter_rotation_t initial_rotation,
    const meter_orientation_policy_config_t *config)
{
    if (!policy || !config) {
        return;
    }

    *policy = (meter_orientation_policy_t){
        .config = *config,
        .current = initial_rotation,
        .candidate = METER_ROTATION_INVALID,
    };
}

bool meter_orientation_policy_update(
    meter_orientation_policy_t *policy,
    int32_t x_mg,
    int32_t y_mg,
    int32_t z_mg,
    uint32_t now_ms,
    meter_rotation_t *committed_rotation)
{
    if (!policy || !committed_rotation || !sample_is_gravity(x_mg, y_mg, z_mg)) {
        if (policy) {
            policy->filter_valid = false;
            reset_candidate(policy);
        }
        return false;
    }

    if (policy->filter_valid &&
        now_ms - policy->last_sample_ms > policy->config.maximum_sample_gap_ms) {
        policy->filter_valid = false;
        reset_candidate(policy);
    }

    if (!policy->filter_valid) {
        policy->filtered_x_mg = x_mg;
        policy->filtered_y_mg = y_mg;
        policy->filtered_z_mg = z_mg;
        policy->filter_valid = true;
    } else {
        policy->filtered_x_mg =
            (policy->filtered_x_mg * METER_FILTER_OLD_WEIGHT + x_mg) /
            METER_FILTER_TOTAL_WEIGHT;
        policy->filtered_y_mg =
            (policy->filtered_y_mg * METER_FILTER_OLD_WEIGHT + y_mg) /
            METER_FILTER_TOTAL_WEIGHT;
        policy->filtered_z_mg =
            (policy->filtered_z_mg * METER_FILTER_OLD_WEIGHT + z_mg) /
            METER_FILTER_TOTAL_WEIGHT;
    }
    policy->last_sample_ms = now_ms;

    bool ambiguous = false;
    const meter_rotation_t observed = classify_rotation(policy, &ambiguous);
    if (observed == METER_ROTATION_INVALID) {
        if (!ambiguous || policy->candidate == METER_ROTATION_INVALID ||
            now_ms - policy->candidate_last_supported_ms >
                policy->config.ambiguity_grace_ms) {
            reset_candidate(policy);
        }
        return false;
    }
    if (observed == policy->current) {
        reset_candidate(policy);
        return false;
    }

    if (observed != policy->candidate) {
        policy->candidate = observed;
        policy->candidate_since_ms = now_ms;
        policy->candidate_last_supported_ms = now_ms;
        policy->candidate_samples = 1;
        return false;
    }

    policy->candidate_last_supported_ms = now_ms;
    if (policy->candidate_samples < UINT16_MAX) {
        ++policy->candidate_samples;
    }

    const uint32_t stable_for_ms = now_ms - policy->candidate_since_ms;
    if (policy->candidate_samples < policy->config.minimum_samples ||
        stable_for_ms < policy->config.stable_ms) {
        return false;
    }

    policy->current = observed;
    *committed_rotation = observed;
    reset_candidate(policy);
    return true;
}
