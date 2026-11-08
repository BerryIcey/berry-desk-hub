#pragma once

#include "esp_err.h"

typedef enum {
    METER_ACCOUNT_BUTTON_PREVIOUS = -1,
    METER_ACCOUNT_BUTTON_NEXT = 1,
} meter_account_button_action_t;

typedef void (*meter_account_button_callback_t)(
    meter_account_button_action_t action,
    void *context);

esp_err_t meter_buttons_start(
    meter_account_button_callback_t callback,
    void *context);
