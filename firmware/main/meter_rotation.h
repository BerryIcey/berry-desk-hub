#pragma once

typedef enum {
    METER_ROTATION_0 = 0,
    METER_ROTATION_90,
    METER_ROTATION_180,
    METER_ROTATION_270,
    METER_ROTATION_INVALID,
} meter_rotation_t;

meter_rotation_t meter_rotation_from_qmi(meter_rotation_t rotation);
