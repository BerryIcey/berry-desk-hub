#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#define METER_MAX_ACCOUNTS 4
#define METER_ACCOUNT_ID_SIZE 33
#define METER_ACCOUNT_LABEL_SIZE 65

typedef enum {
    METER_LINK_SETUP = 0,
    METER_LINK_CONNECTING,
    METER_LINK_DISCOVERING,
    METER_LINK_LIVE,
    METER_LINK_STALE,
    METER_LINK_OFFLINE,
    METER_LINK_AUTH_REQUIRED,
} meter_link_state_t;

typedef enum {
    METER_SOURCE_NONE = 0,
    METER_SOURCE_WIFI,
    METER_SOURCE_BLE,
} meter_transport_source_t;

typedef struct {
    bool valid;
    meter_link_state_t link_state;
    int used;
    int remaining;
    int window_mins;
    int resets_in;
    int extra_remaining;
    int extra_resets_in;
    int64_t captured_at;
    char id[METER_ACCOUNT_ID_SIZE];
    char label[METER_ACCOUNT_LABEL_SIZE];
    char plan[32];
    char extra_name[40];
} usage_account_t;

typedef struct {
    bool valid;
    meter_link_state_t link_state;
    meter_transport_source_t source;
    int used;
    int remaining;
    int window_mins;
    int resets_in;
    int extra_remaining;
    int extra_resets_in;
    int next_poll_in;
    int64_t captured_at;
    TickType_t received_tick;
    TickType_t freshness_tick;
    char plan[32];
    char extra_name[40];
    char setup_ssid[33];
    char setup_psk[16];
    uint8_t account_count;
    uint8_t selected_account;
    usage_account_t accounts[METER_MAX_ACCOUNTS];
} usage_snapshot_t;
