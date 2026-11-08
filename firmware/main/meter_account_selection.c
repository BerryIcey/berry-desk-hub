#include "meter_account_selection.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void apply_account(const meter_account_selection_t *selection,
                          usage_snapshot_t *snapshot, uint8_t index)
{
    if (!selection || !snapshot || snapshot->account_count == 0 ||
        index >= snapshot->account_count || index >= METER_MAX_ACCOUNTS) {
        return;
    }

    const usage_account_t *account = &snapshot->accounts[index];
    snapshot->selected_account = index;
    snapshot->valid = account->valid;
    snapshot->link_state = account->link_state;
    snapshot->used = account->used;
    snapshot->remaining = account->remaining;
    snapshot->window_mins = account->window_mins;
    snapshot->resets_in = account->resets_in;
    snapshot->extra_remaining = account->extra_remaining;
    snapshot->extra_resets_in = account->extra_resets_in;
    snapshot->captured_at = account->captured_at;
    snapshot->freshness_tick = selection->generation_freshness_tick;
    if (account->captured_at > 0 &&
        selection->generation_at > account->captured_at) {
        uint64_t age_seconds =
            (uint64_t)(selection->generation_at - account->captured_at);
        uint64_t age_ticks = age_seconds * (uint64_t)configTICK_RATE_HZ;
        if (age_ticks > UINT32_MAX) {
            age_ticks = UINT32_MAX;
        }
        /* Tick subtraction intentionally keeps FreeRTOS wraparound semantics. */
        snapshot->freshness_tick -= (TickType_t)age_ticks;
    }
    snprintf(snapshot->plan, sizeof(snapshot->plan), "%s", account->plan);
    snprintf(snapshot->extra_name, sizeof(snapshot->extra_name), "%s",
             account->extra_name);
}

void meter_account_selection_init(meter_account_selection_t *selection)
{
    if (selection) {
        memset(selection, 0, sizeof(*selection));
    }
}

void meter_account_selection_reconcile(
    meter_account_selection_t *selection,
    usage_snapshot_t *snapshot)
{
    if (!selection || !snapshot || snapshot->account_count == 0) {
        return;
    }
    if (snapshot->account_count > METER_MAX_ACCOUNTS) {
        snapshot->account_count = METER_MAX_ACCOUNTS;
    }
    selection->generation_at = snapshot->captured_at;
    selection->generation_freshness_tick = snapshot->freshness_tick;

    uint8_t selected = 0;
    if (selection->selected_id[0] != '\0') {
        for (uint8_t index = 0; index < snapshot->account_count; ++index) {
            if (strcmp(selection->selected_id, snapshot->accounts[index].id) == 0) {
                selected = index;
                break;
            }
        }
    }

    snprintf(selection->selected_id, sizeof(selection->selected_id), "%s",
             snapshot->accounts[selected].id);
    apply_account(selection, snapshot, selected);
}

bool meter_account_selection_step(
    meter_account_selection_t *selection,
    usage_snapshot_t *snapshot,
    int direction)
{
    if (!selection || !snapshot || snapshot->account_count <= 1 || direction == 0) {
        return false;
    }
    if (snapshot->account_count > METER_MAX_ACCOUNTS) {
        snapshot->account_count = METER_MAX_ACCOUNTS;
    }

    int count = snapshot->account_count;
    int selected = snapshot->selected_account < snapshot->account_count
                       ? snapshot->selected_account
                       : 0;
    selected = direction > 0 ? (selected + 1) % count
                             : (selected + count - 1) % count;
    snprintf(selection->selected_id, sizeof(selection->selected_id), "%s",
             snapshot->accounts[selected].id);
    apply_account(selection, snapshot, (uint8_t)selected);
    return true;
}

void meter_account_selection_apply_transport_link(
    usage_snapshot_t *snapshot,
    meter_link_state_t transport_link)
{
    if (!snapshot) {
        return;
    }

    /* A broken transport must never look LIVE merely because the selected
     * account's last cached entry was healthy. Once transport is live again,
     * expose the selected account's own stale/auth/online state. */
    if (transport_link == METER_LINK_LIVE && snapshot->account_count > 0 &&
        snapshot->account_count <= METER_MAX_ACCOUNTS &&
        snapshot->selected_account < snapshot->account_count) {
        snapshot->link_state =
            snapshot->accounts[snapshot->selected_account].link_state;
        return;
    }
    snapshot->link_state = transport_link;
}
