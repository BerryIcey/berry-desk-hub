#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t berry_serial_start(void);
bool berry_serial_get(char *destination, size_t capacity);

/* Accepts validated data from either transport; owned copy is kept internally. */
bool berry_snapshot_publish(const char *body);

unsigned berry_serial_last_age_seconds(void);
