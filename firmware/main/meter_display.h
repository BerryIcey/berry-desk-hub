#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"
#include "meter_rotation.h"

lv_display_t *meter_display_start(void);

/*
 * Configure the touch input registered by meter_display_start(). Call this
 * while holding the LVGL/display lock. A zero or non-LVGL-compatible timeout
 * is rejected, as is a call made before the touch input exists.
 */
esp_err_t meter_display_set_touch_long_press_time(uint32_t milliseconds);

/*
 * Apply a hardware rotation while running in the LVGL task context. The
 * registered touch callback follows the same rotation automatically.
 */
esp_err_t meter_display_set_rotation(meter_rotation_t rotation);
