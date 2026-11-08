#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "meter_rotation.h"

esp_err_t meter_orientation_start(void);

/* Returns the latest stable rotation without consuming it. */
bool meter_orientation_get_rotation(meter_rotation_t *rotation);

/* Returns a non-consuming generation that advances throughout motion. */
bool meter_orientation_get_motion_generation(uint32_t *generation);
