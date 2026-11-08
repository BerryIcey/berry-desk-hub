#include "meter_activity_policy.h"

#include <stddef.h>

void meter_activity_policy_init(
    meter_activity_policy_t *policy,
    uint32_t now_ms)
{
    if (!policy) {
        return;
    }

    *policy = (meter_activity_policy_t){
        .last_activity_ms = now_ms,
        .dimmed = false,
    };
}

meter_activity_action_t meter_activity_policy_on_touch(
    meter_activity_policy_t *policy,
    uint32_t now_ms)
{
    if (!policy) {
        return METER_ACTIVITY_NONE;
    }

    policy->last_activity_ms = now_ms;
    return policy->dimmed ? METER_ACTIVITY_WAKE : METER_ACTIVITY_INTERACT;
}

meter_activity_action_t meter_activity_policy_on_motion(
    meter_activity_policy_t *policy,
    uint32_t now_ms)
{
    if (!policy) {
        return METER_ACTIVITY_NONE;
    }

    policy->last_activity_ms = now_ms;
    return policy->dimmed ? METER_ACTIVITY_WAKE : METER_ACTIVITY_NONE;
}

meter_activity_action_t meter_activity_policy_on_timer(
    const meter_activity_policy_t *policy,
    uint32_t now_ms,
    uint32_t idle_timeout_ms)
{
    if (!policy || policy->dimmed || idle_timeout_ms == 0) {
        return METER_ACTIVITY_NONE;
    }

    return now_ms - policy->last_activity_ms >= idle_timeout_ms
               ? METER_ACTIVITY_DIM
               : METER_ACTIVITY_NONE;
}

void meter_activity_policy_commit(
    meter_activity_policy_t *policy,
    meter_activity_action_t action)
{
    if (!policy) {
        return;
    }

    if (action == METER_ACTIVITY_WAKE) {
        policy->dimmed = false;
    } else if (action == METER_ACTIVITY_DIM) {
        policy->dimmed = true;
    }
}

bool meter_activity_policy_is_dimmed(const meter_activity_policy_t *policy)
{
    return policy && policy->dimmed;
}
