#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
esp_err_t berry_pickup_start(void);
bool berry_pickup_stationary(void);
void berry_pickup_diagnostics(int *status,unsigned *samples,unsigned *wakes,unsigned *flags,int *x,int *y,int *z,unsigned *polls,int *read_status,unsigned *stamp);
