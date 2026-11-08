#include "transport_manager_policy.h"

meter_link_state_t meter_transport_policy_aggregate(
    const meter_transport_policy_t *policy)
{
    bool has_auth = false;
    bool has_discovering = false;
    bool has_connecting = false;

    for (int index = METER_SOURCE_WIFI; index <= METER_SOURCE_BLE; ++index) {
        if (!policy->source_enabled[index]) {
            continue;
        }
        meter_link_state_t state = policy->source_state[index];
        if (state == METER_LINK_LIVE) {
            return METER_LINK_LIVE;
        }
        has_auth |= state == METER_LINK_AUTH_REQUIRED;
        has_discovering |= state == METER_LINK_DISCOVERING;
        has_connecting |= state == METER_LINK_CONNECTING;
    }

    /* A ready local setup portal outranks cached data, but never a live source. */
    if (policy->setup_active) {
        return METER_LINK_SETUP;
    }
    if (policy->has_snapshot) {
        return METER_LINK_STALE;
    }
    if (has_auth) {
        return METER_LINK_AUTH_REQUIRED;
    }
    if (has_discovering) {
        return METER_LINK_DISCOVERING;
    }
    if (has_connecting) {
        return METER_LINK_CONNECTING;
    }
    return METER_LINK_OFFLINE;
}
