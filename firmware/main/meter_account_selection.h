#pragma once

#include <stdbool.h>

#include "usage_model.h"

typedef struct {
    char selected_id[METER_ACCOUNT_ID_SIZE];
    int64_t generation_at;
    TickType_t generation_freshness_tick;
} meter_account_selection_t;

void meter_account_selection_init(meter_account_selection_t *selection);

/** Preserve the stable selected ID across a newly received account list. */
void meter_account_selection_reconcile(
    meter_account_selection_t *selection,
    usage_snapshot_t *snapshot);

/** Move by direction (-1 previous, +1 next), wrapping at the list edges. */
bool meter_account_selection_step(
    meter_account_selection_t *selection,
    usage_snapshot_t *snapshot,
    int direction);

/** Combine the physical transport state with the selected account state. */
void meter_account_selection_apply_transport_link(
    usage_snapshot_t *snapshot,
    meter_link_state_t transport_link);
