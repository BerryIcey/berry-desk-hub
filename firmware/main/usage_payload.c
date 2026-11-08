#include "usage_payload.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "freertos/task.h"

#define HOST_REFRESH_FALLBACK_SECONDS 60

static int clamp_percent(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return value;
}

static int json_int(const cJSON *object, const char *name, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static void json_string_copy(const cJSON *object, const char *name, char *target, size_t size,
                             const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    const char *value = cJSON_IsString(item) && item->valuestring ? item->valuestring : fallback;
    snprintf(target, size, "%s", value ? value : "");
}

static meter_link_state_t link_state_from_status(const cJSON *object, bool valid)
{
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(object, "status");
    const char *value =
        cJSON_IsString(status) && status->valuestring ? status->valuestring : "";
    if (valid && (strcmp(value, "ok") == 0 ||
                  strcmp(value, "refreshing") == 0)) {
        return METER_LINK_LIVE;
    }
    if (strcmp(value, "auth_required") == 0 ||
        strcmp(value, "authentication_required") == 0 ||
        strcmp(value, "pairing") == 0) {
        return METER_LINK_AUTH_REQUIRED;
    }
    if (!valid && (strcmp(value, "starting") == 0 ||
                   strcmp(value, "refreshing") == 0)) {
        return METER_LINK_CONNECTING;
    }
    if (!valid && (strcmp(value, "offline") == 0 ||
                   strcmp(value, "error") == 0)) {
        return METER_LINK_OFFLINE;
    }
    return METER_LINK_STALE;
}

static void account_plan_copy(const cJSON *object, char *target, size_t size,
                              const char *fallback)
{
    const cJSON *plan_label = cJSON_GetObjectItemCaseSensitive(object, "planLabel");
    if (cJSON_IsString(plan_label) && plan_label->valuestring &&
        plan_label->valuestring[0]) {
        snprintf(target, size, "%s", plan_label->valuestring);
        return;
    }

    const cJSON *metadata = cJSON_GetObjectItemCaseSensitive(object, "metadata");
    plan_label = cJSON_IsObject(metadata)
                     ? cJSON_GetObjectItemCaseSensitive(metadata, "planLabel")
                     : NULL;
    if (cJSON_IsString(plan_label) && plan_label->valuestring &&
        plan_label->valuestring[0]) {
        snprintf(target, size, "%s", plan_label->valuestring);
        return;
    }

    const cJSON *plan = cJSON_GetObjectItemCaseSensitive(object, "plan");
    if (cJSON_IsString(plan) && plan->valuestring && plan->valuestring[0]) {
        snprintf(target, size, "%s", plan->valuestring);
        return;
    }
    snprintf(target, size, "%s", fallback ? fallback : "");
}

static void parse_extra(const cJSON *object, usage_account_t *account)
{
    account->extra_remaining = -1;
    account->extra_resets_in = -1;
    snprintf(account->extra_name, sizeof(account->extra_name), "ADDITIONAL LIMIT");

    const cJSON *extras = cJSON_GetObjectItemCaseSensitive(object, "extras");
    const cJSON *extra = cJSON_IsArray(extras) ? cJSON_GetArrayItem(extras, 0) : NULL;
    if (!cJSON_IsObject(extra)) {
        return;
    }

    json_string_copy(extra, "name", account->extra_name,
                     sizeof(account->extra_name), "EXTRA");
    const cJSON *primary = cJSON_GetObjectItemCaseSensitive(extra, "primary");
    if (!cJSON_IsObject(primary)) {
        return;
    }

    const cJSON *remaining = cJSON_GetObjectItemCaseSensitive(primary, "remaining");
    const cJSON *used = cJSON_GetObjectItemCaseSensitive(primary, "used");
    if (cJSON_IsNumber(remaining)) {
        account->extra_remaining = clamp_percent(remaining->valueint);
        account->extra_resets_in = json_int(primary, "resetsIn", -1);
    } else if (cJSON_IsNumber(used)) {
        account->extra_remaining = 100 - clamp_percent(used->valueint);
        account->extra_resets_in = json_int(primary, "resetsIn", -1);
    }
}

static bool parse_account_usage(const cJSON *object, const char *fallback_plan,
                                usage_account_t *account)
{
    memset(account, 0, sizeof(*account));
    account->resets_in = -1;
    account->extra_remaining = -1;
    account->extra_resets_in = -1;
    account_plan_copy(object, account->plan, sizeof(account->plan), fallback_plan);
    parse_extra(object, account);

    const cJSON *captured_at = cJSON_GetObjectItemCaseSensitive(object, "capturedAt");
    account->captured_at =
        cJSON_IsNumber(captured_at) ? (int64_t)captured_at->valuedouble : 0;

    const cJSON *preferred = cJSON_GetObjectItemCaseSensitive(object, "preferred");
    const cJSON *primary = cJSON_IsObject(preferred)
                               ? cJSON_GetObjectItemCaseSensitive(preferred, "primary")
                               : NULL;
    const cJSON *used = cJSON_IsObject(primary)
                            ? cJSON_GetObjectItemCaseSensitive(primary, "used")
                            : NULL;
    const cJSON *remaining = cJSON_IsObject(primary)
                                 ? cJSON_GetObjectItemCaseSensitive(primary, "remaining")
                                 : NULL;
    const bool has_used = cJSON_IsNumber(used);
    const bool has_remaining = cJSON_IsNumber(remaining);
    account->valid = has_used || has_remaining;
    account->link_state = link_state_from_status(object, account->valid);
    if (!account->valid) {
        return false;
    }

    account->used = has_used ? clamp_percent(used->valueint)
                             : 100 - clamp_percent(remaining->valueint);
    account->remaining = has_remaining ? clamp_percent(remaining->valueint)
                                       : 100 - account->used;
    account->window_mins = json_int(primary, "windowMins", 0);
    account->resets_in = json_int(primary, "resetsIn", -1);
    return true;
}

static void copy_account_usage(const usage_account_t *account, usage_snapshot_t *result)
{
    result->valid = account->valid;
    result->link_state = account->link_state;
    result->used = account->used;
    result->remaining = account->remaining;
    result->window_mins = account->window_mins;
    result->resets_in = account->resets_in;
    result->extra_remaining = account->extra_remaining;
    result->extra_resets_in = account->extra_resets_in;
    result->captured_at = account->captured_at;
    snprintf(result->plan, sizeof(result->plan), "%s", account->plan);
    snprintf(result->extra_name, sizeof(result->extra_name), "%s",
             account->extra_name);
}

static bool parse_accounts(const cJSON *root, const char *fallback_plan,
                           usage_snapshot_t *result)
{
    const cJSON *accounts = cJSON_GetObjectItemCaseSensitive(root, "accounts");
    if (!accounts) {
        return true;
    }
    if (!cJSON_IsArray(accounts)) {
        return false;
    }

    int count = cJSON_GetArraySize(accounts);
    if (count < 0 || count > METER_MAX_ACCOUNTS) {
        return false;
    }
    for (int index = 0; index < count; ++index) {
        const cJSON *item = cJSON_GetArrayItem(accounts, index);
        const cJSON *id = cJSON_IsObject(item)
                              ? cJSON_GetObjectItemCaseSensitive(item, "id")
                              : NULL;
        const cJSON *label = cJSON_IsObject(item)
                                 ? cJSON_GetObjectItemCaseSensitive(item, "label")
                                 : NULL;
        if (!cJSON_IsString(id) || !id->valuestring || !id->valuestring[0] ||
            strlen(id->valuestring) >= METER_ACCOUNT_ID_SIZE ||
            !cJSON_IsString(label) || !label->valuestring || !label->valuestring[0] ||
            strlen(label->valuestring) >= METER_ACCOUNT_LABEL_SIZE) {
            return false;
        }
        for (int previous = 0; previous < index; ++previous) {
            if (strcmp(result->accounts[previous].id, id->valuestring) == 0) {
                return false;
            }
        }

        usage_account_t *account = &result->accounts[index];
        (void)parse_account_usage(item, fallback_plan, account);
        snprintf(account->id, sizeof(account->id), "%s", id->valuestring);
        snprintf(account->label, sizeof(account->label), "%s", label->valuestring);
    }
    result->account_count = (uint8_t)count;
    result->selected_account = 0;
    return true;
}

bool meter_usage_payload_parse(const char *json, usage_snapshot_t *result)
{
    if (!json || !result) {
        return false;
    }

    bool success = false;
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithOpts(json, &parse_end, true);
    if (!root) {
        return false;
    }

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (!cJSON_IsObject(root) || !cJSON_IsNumber(version) || version->valueint != 1) {
        goto cleanup;
    }

    memset(result, 0, sizeof(*result));
    usage_account_t top_level;
    bool top_level_valid = parse_account_usage(root, "", &top_level);
    copy_account_usage(&top_level, result);
    result->next_poll_in = json_int(root, "nextPollIn", HOST_REFRESH_FALLBACK_SECONDS);
    result->received_tick = xTaskGetTickCount();
    result->freshness_tick = result->received_tick;
    if (!parse_accounts(root, top_level.plan, result)) {
        goto cleanup;
    }
    if (!top_level_valid && result->account_count == 0) {
        goto cleanup;
    }

    success = true;

cleanup:
    cJSON_Delete(root);
    return success;
}
