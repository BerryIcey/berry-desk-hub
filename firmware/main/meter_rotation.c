#include "meter_rotation.h"

meter_rotation_t meter_rotation_from_qmi(meter_rotation_t rotation)
{
    if (rotation > METER_ROTATION_270) {
        return METER_ROTATION_INVALID;
    }

    /*
     * Board-level alignment confirmed on hardware: the logical display
     * direction is 90 degrees clockwise from the QMI8658 direction.
     */
    return (meter_rotation_t)(((unsigned)rotation + 1U) % 4U);
}
