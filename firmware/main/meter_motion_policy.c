#include "meter_motion_policy.h"

#include <stddef.h>

#define METER_ACCEL_PLAUSIBLE_MAX_MG 4000
#define METER_BASELINE_DIVISOR 32

static int64_t absolute_value(int32_t value)
{
    return value < 0 ? -(int64_t)value : value;
}

static bool sample_is_plausible(int32_t x_mg, int32_t y_mg, int32_t z_mg)
{
    return absolute_value(x_mg) <= METER_ACCEL_PLAUSIBLE_MAX_MG &&
           absolute_value(y_mg) <= METER_ACCEL_PLAUSIBLE_MAX_MG &&
           absolute_value(z_mg) <= METER_ACCEL_PLAUSIBLE_MAX_MG;
}

static bool vector_exceeds_threshold(
    int32_t x_mg,
    int32_t y_mg,
    int32_t z_mg,
    uint16_t threshold_mg)
{
    const int64_t magnitude_squared =
        (int64_t)x_mg * x_mg + (int64_t)y_mg * y_mg + (int64_t)z_mg * z_mg;
    const int64_t threshold_squared = (int64_t)threshold_mg * threshold_mg;
    return magnitude_squared >= threshold_squared;
}

static bool fixed_point_vector_exceeds_threshold(
    int32_t x_q5,
    int32_t y_q5,
    int32_t z_q5,
    uint16_t threshold_mg)
{
    const int64_t magnitude_squared =
        (int64_t)x_q5 * x_q5 + (int64_t)y_q5 * y_q5 +
        (int64_t)z_q5 * z_q5;
    const int64_t threshold_q5 =
        (int64_t)threshold_mg * METER_BASELINE_DIVISOR;
    return magnitude_squared >= threshold_q5 * threshold_q5;
}

static void establish_baseline(
    meter_motion_policy_t *policy,
    int32_t x_mg,
    int32_t y_mg,
    int32_t z_mg,
    uint32_t now_ms)
{
    policy->previous_x_mg = x_mg;
    policy->previous_y_mg = y_mg;
    policy->previous_z_mg = z_mg;
    policy->baseline_x_mg = x_mg;
    policy->baseline_y_mg = y_mg;
    policy->baseline_z_mg = z_mg;
    policy->baseline_x_q5 = x_mg * METER_BASELINE_DIVISOR;
    policy->baseline_y_q5 = y_mg * METER_BASELINE_DIVISOR;
    policy->baseline_z_q5 = z_mg * METER_BASELINE_DIVISOR;
    policy->last_sample_ms = now_ms;
    policy->sample_valid = true;
    policy->active = false;
}

void meter_motion_policy_init(
    meter_motion_policy_t *policy,
    const meter_motion_policy_config_t *config)
{
    if (!policy || !config) {
        return;
    }

    *policy = (meter_motion_policy_t){
        .config = *config,
    };
}

bool meter_motion_policy_update(
    meter_motion_policy_t *policy,
    int32_t x_mg,
    int32_t y_mg,
    int32_t z_mg,
    uint32_t now_ms)
{
    if (!policy || policy->config.threshold_mg == 0 ||
        !sample_is_plausible(x_mg, y_mg, z_mg)) {
        if (policy) {
            policy->sample_valid = false;
            policy->active = false;
        }
        return false;
    }

    if (!policy->sample_valid ||
        now_ms - policy->last_sample_ms > policy->config.maximum_sample_gap_ms) {
        establish_baseline(policy, x_mg, y_mg, z_mg, now_ms);
        return false;
    }

    const int32_t jerk_x = x_mg - policy->previous_x_mg;
    const int32_t jerk_y = y_mg - policy->previous_y_mg;
    const int32_t jerk_z = z_mg - policy->previous_z_mg;
    const int32_t baseline_x_q5 =
        x_mg * METER_BASELINE_DIVISOR - policy->baseline_x_q5;
    const int32_t baseline_y_q5 =
        y_mg * METER_BASELINE_DIVISOR - policy->baseline_y_q5;
    const int32_t baseline_z_q5 =
        z_mg * METER_BASELINE_DIVISOR - policy->baseline_z_q5;

    const bool detected =
        vector_exceeds_threshold(
            jerk_x, jerk_y, jerk_z, policy->config.threshold_mg) ||
        fixed_point_vector_exceeds_threshold(
            baseline_x_q5, baseline_y_q5, baseline_z_q5,
            policy->config.threshold_mg);

    policy->previous_x_mg = x_mg;
    policy->previous_y_mg = y_mg;
    policy->previous_z_mg = z_mg;
    policy->baseline_x_q5 += baseline_x_q5 / METER_BASELINE_DIVISOR;
    policy->baseline_y_q5 += baseline_y_q5 / METER_BASELINE_DIVISOR;
    policy->baseline_z_q5 += baseline_z_q5 / METER_BASELINE_DIVISOR;
    policy->baseline_x_mg = policy->baseline_x_q5 / METER_BASELINE_DIVISOR;
    policy->baseline_y_mg = policy->baseline_y_q5 / METER_BASELINE_DIVISOR;
    policy->baseline_z_mg = policy->baseline_z_q5 / METER_BASELINE_DIVISOR;
    policy->last_sample_ms = now_ms;

    if (detected) {
        policy->last_detected_ms = now_ms;
        policy->active = true;
    } else if (policy->active &&
               now_ms - policy->last_detected_ms >= policy->config.active_hold_ms) {
        policy->active = false;
    }

    return policy->active;
}
