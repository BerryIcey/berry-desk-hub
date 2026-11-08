#include "meter_battery.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define AXP2101_I2C_ADDRESS 0x34
#define AXP2101_CHIP_ID 0x4A

#define AXP2101_REG_STATUS1 0x00
#define AXP2101_REG_STATUS2 0x01
#define AXP2101_REG_IC_TYPE 0x03
#define AXP2101_REG_ADC_CHANNEL_CTRL 0x30
#define AXP2101_REG_BATTERY_VOLTAGE_H 0x34
#define AXP2101_REG_BATTERY_DETECT_CTRL 0x68
#define AXP2101_REG_BATTERY_PERCENT 0xA4

#define AXP2101_STATUS1_VBUS_GOOD (1U << 5)
#define AXP2101_STATUS1_BATTERY_PRESENT (1U << 3)
#define AXP2101_STATUS2_SOURCE_MASK (3U << 5)
#define AXP2101_STATUS2_CHARGING (1U << 5)
#define AXP2101_STATUS2_VBUS_NOT_PRESENT (1U << 3)
#define AXP2101_ADC_BATTERY_VOLTAGE_ENABLE (1U << 0)
#define AXP2101_ADC_TS_PIN_ENABLE (1U << 1)
#define AXP2101_BATTERY_DETECT_ENABLE (1U << 0)

#define AXP2101_I2C_TIMEOUT_MS 100
#define BATTERY_TASK_STACK_SIZE 3072
#define BATTERY_TASK_PRIORITY 4
#define BATTERY_FAILURE_LOG_INTERVAL 12U

static const char *TAG = "meter_battery";

static i2c_master_dev_handle_t s_axp;
static TaskHandle_t s_task;
static meter_battery_update_cb_t s_callback;
static void *s_callback_context;

static uint32_t monotonic_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t read_registers(uint8_t address, uint8_t *data, size_t size)
{
    if (!s_axp || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(
        s_axp, &address, sizeof(address), data, size, AXP2101_I2C_TIMEOUT_MS);
}

static esp_err_t read_register(uint8_t address, uint8_t *value)
{
    return read_registers(address, value, 1);
}

static esp_err_t write_register(uint8_t address, uint8_t value)
{
    const uint8_t payload[] = {address, value};
    return i2c_master_transmit(
        s_axp, payload, sizeof(payload), AXP2101_I2C_TIMEOUT_MS);
}

static esp_err_t update_register(
    uint8_t address,
    uint8_t clear_mask,
    uint8_t set_mask)
{
    uint8_t value = 0;
    esp_err_t result = read_register(address, &value);
    if (result != ESP_OK) {
        return result;
    }
    value = (uint8_t)((value & (uint8_t)~clear_mask) | set_mask);
    return write_register(address, value);
}

static esp_err_t configure_measurements(void)
{
    /*
     * Waveshare's official example enables battery-voltage measurement and
     * disables TS-pin measurement because this board has no battery
     * temperature-sense connection. Preserve every charger-current and
     * charge-voltage register: this module only observes battery state.
     */
    esp_err_t result = update_register(
        AXP2101_REG_ADC_CHANNEL_CTRL,
        AXP2101_ADC_TS_PIN_ENABLE,
        AXP2101_ADC_BATTERY_VOLTAGE_ENABLE);
    if (result != ESP_OK) {
        return result;
    }
    return update_register(
        AXP2101_REG_BATTERY_DETECT_CTRL,
        0,
        AXP2101_BATTERY_DETECT_ENABLE);
}

static esp_err_t read_snapshot(meter_battery_snapshot_t *snapshot)
{
    if (!snapshot) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status[2] = {0};
    esp_err_t result = read_registers(AXP2101_REG_STATUS1, status, sizeof(status));
    if (result != ESP_OK) {
        return result;
    }

    meter_battery_snapshot_t next = {
        .valid = true,
        .battery_present =
            (status[0] & AXP2101_STATUS1_BATTERY_PRESENT) != 0,
        .charging =
            (status[1] & AXP2101_STATUS2_SOURCE_MASK) ==
            AXP2101_STATUS2_CHARGING,
        .vbus_present =
            (status[0] & AXP2101_STATUS1_VBUS_GOOD) != 0 &&
            (status[1] & AXP2101_STATUS2_VBUS_NOT_PRESENT) == 0,
        .percent = -1,
        .voltage_mv = 0,
        .sampled_at_ms = monotonic_ms(),
    };

    if (next.battery_present) {
        uint8_t percent = 0;
        uint8_t voltage[2] = {0};
        result = read_register(AXP2101_REG_BATTERY_PERCENT, &percent);
        if (result != ESP_OK) {
            return result;
        }
        result = read_registers(
            AXP2101_REG_BATTERY_VOLTAGE_H, voltage, sizeof(voltage));
        if (result != ESP_OK) {
            return result;
        }

        next.percent = percent <= 100 ? percent : -1;
        next.voltage_mv =
            (uint16_t)(((uint16_t)(voltage[0] & 0x1FU) << 8) | voltage[1]);
    } else {
        next.charging = false;
    }

    *snapshot = next;
    return ESP_OK;
}

static bool state_changed(
    const meter_battery_snapshot_t *previous,
    const meter_battery_snapshot_t *current)
{
    return previous->valid != current->valid ||
           previous->battery_present != current->battery_present ||
           previous->charging != current->charging ||
           previous->vbus_present != current->vbus_present ||
           previous->percent != current->percent;
}

static void battery_task(void *argument)
{
    (void)argument;
    meter_battery_snapshot_t previous = {
        .valid = false,
        .percent = -1,
    };
    uint32_t failure_count = 0;
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        meter_battery_snapshot_t current;
        esp_err_t result = read_snapshot(&current);
        if (result == ESP_OK) {
            failure_count = 0;
            if (state_changed(&previous, &current)) {
                if (current.battery_present && current.percent >= 0) {
                    ESP_LOGI(
                        TAG, "Battery %d%%, %u mV, %s",
                        current.percent,
                        current.voltage_mv,
                        current.charging ? "charging" : "not charging");
                } else {
                    ESP_LOGI(
                        TAG, "Battery not detected, external power %s",
                        current.vbus_present ? "present" : "absent");
                }
            }
            previous = current;
            if (s_callback) {
                s_callback(&current, s_callback_context);
            }
        } else {
            ++failure_count;
            if (failure_count == 1 ||
                failure_count % BATTERY_FAILURE_LOG_INTERVAL == 0) {
                ESP_LOGW(
                    TAG, "AXP2101 sample failed (%lu consecutive): %s",
                    (unsigned long)failure_count,
                    esp_err_to_name(result));
            }
        }

        xTaskDelayUntil(
            &last_wake,
            pdMS_TO_TICKS(CONFIG_METER_BATTERY_POLL_SECONDS * 1000));
    }
}

esp_err_t meter_battery_start(meter_battery_update_cb_t callback, void *context)
{
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task || s_axp) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = bsp_i2c_init();
    if (result != ESP_OK) {
        return result;
    }
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDRESS,
        .scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ,
    };
    result = i2c_master_bus_add_device(bus, &device_config, &s_axp);
    if (result != ESP_OK) {
        s_axp = NULL;
        return result;
    }

    uint8_t chip_id = 0;
    result = read_register(AXP2101_REG_IC_TYPE, &chip_id);
    if (result == ESP_OK && chip_id != AXP2101_CHIP_ID) {
        ESP_LOGE(TAG, "Unexpected PMIC chip ID 0x%02x", chip_id);
        result = ESP_ERR_NOT_FOUND;
    }
    if (result == ESP_OK) {
        result = configure_measurements();
    }
    if (result != ESP_OK) {
        (void)i2c_master_bus_rm_device(s_axp);
        s_axp = NULL;
        return result;
    }

    s_callback = callback;
    s_callback_context = context;
    if (xTaskCreate(
            battery_task,
            "meter_battery",
            BATTERY_TASK_STACK_SIZE,
            NULL,
            BATTERY_TASK_PRIORITY,
            &s_task) != pdPASS) {
        s_callback = NULL;
        s_callback_context = NULL;
        (void)i2c_master_bus_rm_device(s_axp);
        s_axp = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG, "AXP2101 detected; sampling every %d seconds",
        CONFIG_METER_BATTERY_POLL_SECONDS);
    return ESP_OK;
}
