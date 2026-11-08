#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    METER_ACTIVITY_NONE = 0,
    METER_ACTIVITY_WAKE,
    METER_ACTIVITY_INTERACT,
    METER_ACTIVITY_DIM,
} meter_activity_action_t;

typedef struct {
    uint32_t last_activity_ms;
    bool dimmed;
} meter_activity_policy_t;

void meter_activity_policy_init(
    meter_activity_policy_t *policy,
    uint32_t now_ms);

meter_activity_action_t meter_activity_policy_on_touch(
    meter_activity_policy_t *policy,
    uint32_t now_ms);

meter_activity_action_t meter_activity_policy_on_motion(
    meter_activity_policy_t *policy,
    uint32_t now_ms);

meter_activity_action_t meter_activity_policy_on_timer(
    const meter_activity_policy_t *policy,
    uint32_t now_ms,
    uint32_t idle_timeout_ms);

/* Commit only after the corresponding hardware brightness change succeeds. */
void meter_activity_policy_commit(
    meter_activity_policy_t *policy,
    meter_activity_action_t action);

bool meter_activity_policy_is_dimmed(const meter_activity_policy_t *policy);
