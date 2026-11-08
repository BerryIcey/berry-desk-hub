#pragma once
#include <stdint.h>
static inline uint16_t berry_spinner_angle(uint32_t tick_ms) {
    return (uint16_t)(((tick_ms % 1600U) * 360U) / 1600U);
}
