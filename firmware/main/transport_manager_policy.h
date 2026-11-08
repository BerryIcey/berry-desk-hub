#pragma once

#include <stdbool.h>

#include "usage_model.h"

typedef struct {
    meter_link_state_t source_state[3];
    bool source_enabled[3];
    bool has_snapshot;
    bool setup_active;
} meter_transport_policy_t;

/** Computes the user-visible aggregate link state for the current sources. */
meter_link_state_t meter_transport_policy_aggregate(
    const meter_transport_policy_t *policy);
