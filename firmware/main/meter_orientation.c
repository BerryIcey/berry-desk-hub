#include "meter_orientation.h"

#include <stdint.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "meter_motion_policy.h"
#include "meter_orientation_policy.h"
#include "qmi8658.h"
#include "sdkconfig.h"

#if CONFIG_METER_AUTO_ROTATE || CONFIG_METER_MOTION_WAKE

#define METER_ORIENTATION_TASK_STACK 4096
#define METER_ORIENTATION_TASK_PRIORITY 3
/* Accelerometer LPF mode 3: 13.37% of 31.25 Hz, about 4.2 Hz. */
#define METER_ACCEL_FILTER_CTRL5 0x07
#define METER_ORIENTATION_LOG_EVERY_FAILURES 20
#define METER_ORIENTATION_NO_DATA_WARN_CYCLES 20
#define METER_ORIENTATION_NO_DATA_REPEAT_CYCLES 200

#if CONFIG_METER_AUTO_ROTATE
#define METER_ORIENTATION_MINIMUM_SAMPLES 6
#define METER_ORIENTATION_DOMINANCE_MARGIN_MG 120
#define METER_ORIENTATION_AMBIGUITY_GRACE_MS 150
#endif

static const char *TAG = "meter_orientation";
static qmi8658_dev_t s_sensor;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

#if CONFIG_METER_AUTO_ROTATE
static meter_rotation_t s_requested_rotation = METER_ROTATION_INVALID;
static bool s_rotation_available;

static const char *rotation_name(meter_rotation_t rotation)
{
    switch (rotation) {
    case METER_ROTATION_0:
        return "0";
    case METER_ROTATION_90:
        return "90";
    case METER_ROTATION_180:
        return "180";
    case METER_ROTATION_270:
        return "270";
    default:
        return "invalid";
    }
}
#endif

static bool convert_mg(float value, int32_t *result)
{
    if (!result || !(value >= -4000.0f && value <= 4000.0f)) {
        return false;
    }
    *result = (int32_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
    return true;
}

#if CONFIG_METER_AUTO_ROTATE
static void publish_rotation(meter_rotation_t rotation)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_requested_rotation = rotation;
    s_rotation_available = true;
    taskEXIT_CRITICAL(&s_state_lock);
}
#endif

#if CONFIG_METER_MOTION_WAKE
static uint32_t s_motion_generation;
static bool s_motion_available;

static void publish_motion(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    ++s_motion_generation;
    s_motion_available = true;
    taskEXIT_CRITICAL(&s_state_lock);
}
#endif

static void qmi_activity_task(void *context)
{
    (void)context;
#if CONFIG_METER_AUTO_ROTATE
    const meter_orientation_policy_config_t orientation_config = {
        .threshold_mg = CONFIG_METER_ROTATION_THRESHOLD_MG,
        .dominance_margin_mg = METER_ORIENTATION_DOMINANCE_MARGIN_MG,
        .stable_ms = CONFIG_METER_ROTATION_STABLE_MS,
        .maximum_sample_gap_ms = CONFIG_METER_ROTATION_SAMPLE_MS * 3,
        .ambiguity_grace_ms = METER_ORIENTATION_AMBIGUITY_GRACE_MS,
        .minimum_samples = METER_ORIENTATION_MINIMUM_SAMPLES,
    };
    meter_orientation_policy_t orientation_policy;
    meter_orientation_policy_init(
        &orientation_policy, METER_ROTATION_INVALID, &orientation_config);
#endif

#if CONFIG_METER_MOTION_WAKE
    const meter_motion_policy_config_t motion_config = {
        .threshold_mg = CONFIG_METER_MOTION_THRESHOLD_MG,
        .maximum_sample_gap_ms = CONFIG_METER_ROTATION_SAMPLE_MS * 3,
        .active_hold_ms = CONFIG_METER_MOTION_HOLD_MS,
    };
    meter_motion_policy_t motion_policy;
    meter_motion_policy_init(&motion_policy, &motion_config);
#endif

    TickType_t last_wake = xTaskGetTickCount();
    uint32_t consecutive_failures = 0;
    uint32_t consecutive_no_data = 0;

    while (true) {
        bool ready = false;
        esp_err_t result = qmi8658_is_data_ready(&s_sensor, &ready);
        if (result == ESP_OK && ready) {
            float x_mg = 0.0f;
            float y_mg = 0.0f;
            float z_mg = 0.0f;
            result = qmi8658_read_accel(&s_sensor, &x_mg, &y_mg, &z_mg);
            if (result == ESP_OK) {
                int32_t x = 0;
                int32_t y = 0;
                int32_t z = 0;
                if (convert_mg(x_mg, &x) && convert_mg(y_mg, &y) &&
                    convert_mg(z_mg, &z)) {
                    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
#if CONFIG_METER_MOTION_WAKE
                    if (meter_motion_policy_update(
                            &motion_policy, x, y, z, now_ms)) {
                        publish_motion();
                    }
#endif
#if CONFIG_METER_AUTO_ROTATE
                    meter_rotation_t detected_rotation;
                    if (meter_orientation_policy_update(
                            &orientation_policy, x, y, z, now_ms,
                            &detected_rotation)) {
                        const meter_rotation_t display_rotation =
                            meter_rotation_from_qmi(detected_rotation);
                        publish_rotation(display_rotation);
                        ESP_LOGI(TAG,
                                 "Stable gravity orientation: sensor=%s, display=%s degrees",
                                 rotation_name(detected_rotation),
                                 rotation_name(display_rotation));
                    }
#endif
                    consecutive_failures = 0;
                    consecutive_no_data = 0;
                } else {
                    result = ESP_ERR_INVALID_RESPONSE;
                }
            }
        } else if (result == ESP_OK) {
            ++consecutive_no_data;
            if (consecutive_no_data == METER_ORIENTATION_NO_DATA_WARN_CYCLES ||
                consecutive_no_data % METER_ORIENTATION_NO_DATA_REPEAT_CYCLES == 0) {
                ESP_LOGW(TAG, "QMI8658 has no fresh accelerometer samples (%lu polls)",
                         (unsigned long)consecutive_no_data);
            }
        }

        if (result != ESP_OK) {
            ++consecutive_failures;
            if (consecutive_failures == 1 ||
                consecutive_failures % METER_ORIENTATION_LOG_EVERY_FAILURES == 0) {
                ESP_LOGW(TAG, "QMI8658 read failed (%lu): %s",
                         (unsigned long)consecutive_failures, esp_err_to_name(result));
            }
        }

        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONFIG_METER_ROTATION_SAMPLE_MS));
    }
}

esp_err_t meter_orientation_start(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGW(TAG, "%s", "BSP I2C bus is unavailable; QMI8658 features disabled");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = qmi8658_init(&s_sensor, bus, QMI8658_ADDRESS_HIGH);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 init failed; sensor-driven display features unavailable: %s",
                 esp_err_to_name(result));
        return result;
    }

    result = qmi8658_set_accel_range(&s_sensor, QMI8658_ACCEL_RANGE_2G);
    if (result == ESP_OK) {
        result = qmi8658_set_accel_odr(&s_sensor, QMI8658_ACCEL_ODR_31_25HZ);
    }
    if (result == ESP_OK) {
        qmi8658_set_accel_unit_mg(&s_sensor, true);
        result = qmi8658_write_register(
            &s_sensor, QMI8658_CTRL5, METER_ACCEL_FILTER_CTRL5);
    }
    if (result == ESP_OK) {
        result = qmi8658_enable_sensors(&s_sensor, QMI8658_ENABLE_ACCEL);
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 configuration failed; sensor features remain disabled: %s",
                 esp_err_to_name(result));
        return result;
    }

    if (xTaskCreate(qmi_activity_task, "meter_qmi",
                    METER_ORIENTATION_TASK_STACK, NULL,
                    METER_ORIENTATION_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "%s", "Could not create QMI8658 activity task");
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_METER_AUTO_ROTATE
    ESP_LOGI(TAG, "Auto rotation enabled (31.25 Hz IMU, %d ms sample, %d ms stability)",
             CONFIG_METER_ROTATION_SAMPLE_MS, CONFIG_METER_ROTATION_STABLE_MS);
#endif
#if CONFIG_METER_MOTION_WAKE
    ESP_LOGI(TAG,
             "Motion wake enabled (%d mg threshold, adaptive baseline, %d ms activity hold)",
             CONFIG_METER_MOTION_THRESHOLD_MG, CONFIG_METER_MOTION_HOLD_MS);
#endif
    return ESP_OK;
}

bool meter_orientation_get_rotation(meter_rotation_t *rotation)
{
#if CONFIG_METER_AUTO_ROTATE
    if (!rotation) {
        return false;
    }

    taskENTER_CRITICAL(&s_state_lock);
    const bool available = s_rotation_available;
    if (available) {
        *rotation = s_requested_rotation;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return available;
#else
    (void)rotation;
    return false;
#endif
}

bool meter_orientation_get_motion_generation(uint32_t *generation)
{
#if CONFIG_METER_MOTION_WAKE
    if (!generation) {
        return false;
    }

    taskENTER_CRITICAL(&s_state_lock);
    const bool available = s_motion_available;
    if (available) {
        *generation = s_motion_generation;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return available;
#else
    (void)generation;
    return false;
#endif
}

#else

esp_err_t meter_orientation_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool meter_orientation_get_rotation(meter_rotation_t *rotation)
{
    (void)rotation;
    return false;
}

bool meter_orientation_get_motion_generation(uint32_t *generation)
{
    (void)generation;
    return false;
}

#endif
