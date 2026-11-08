#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "meter_config.h"

typedef struct {
    char host[64];
    uint16_t port;
    bool discovered;
} meter_endpoint_t;

esp_err_t meter_host_resolve(const meter_config_t *config, meter_endpoint_t *endpoint);
void meter_host_forget(void);
