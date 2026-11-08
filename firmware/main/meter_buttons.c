#include "meter_buttons.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define ACCOUNT_NEXT_GPIO GPIO_NUM_18
#define ACCOUNT_PREVIOUS_GPIO GPIO_NUM_0
#define BUTTON_POLL_MS 20
#define BUTTON_DEBOUNCE_MS 40
#define BUTTON_LONG_PRESS_MS 700
#define BUTTON_TASK_STACK 1536
#define BUTTON_TASK_PRIORITY 4

typedef struct {
    gpio_num_t gpio;
    int sampled_level;
    int stable_level;
    uint32_t sampled_since_ms;
    uint32_t pressed_since_ms;
    bool pressed;
} button_state_t;

static const char *TAG = "meter_buttons";
static meter_account_button_callback_t s_callback;
static void *s_context;
static TaskHandle_t s_task;

static uint32_t monotonic_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool update_button(button_state_t *button, uint32_t now_ms,
                          uint32_t *held_ms)
{
    int level = gpio_get_level(button->gpio);
    if (level != button->sampled_level) {
        button->sampled_level = level;
        button->sampled_since_ms = now_ms;
        return false;
    }
    if (level == button->stable_level ||
        now_ms - button->sampled_since_ms < BUTTON_DEBOUNCE_MS) {
        return false;
    }

    button->stable_level = level;
    if (level == 0) {
        button->pressed = true;
        button->pressed_since_ms = now_ms;
        return false;
    }
    if (!button->pressed) {
        return false;
    }

    button->pressed = false;
    *held_ms = now_ms - button->pressed_since_ms;
    return true;
}

static void buttons_task(void *argument)
{
    (void)argument;
    uint32_t now = monotonic_ms();
#if CONFIG_METER_ACCOUNT_BUTTON_GPIO18
    button_state_t next = {
        .gpio = ACCOUNT_NEXT_GPIO,
        .sampled_level = gpio_get_level(ACCOUNT_NEXT_GPIO),
        .stable_level = gpio_get_level(ACCOUNT_NEXT_GPIO),
        .sampled_since_ms = now,
    };
#endif
#if CONFIG_METER_EXTERNAL_PREVIOUS_BUTTON_GPIO0
    button_state_t previous = {
        .gpio = ACCOUNT_PREVIOUS_GPIO,
        .sampled_level = gpio_get_level(ACCOUNT_PREVIOUS_GPIO),
        .stable_level = gpio_get_level(ACCOUNT_PREVIOUS_GPIO),
        .sampled_since_ms = now,
    };
#endif

    for (;;) {
        now = monotonic_ms();
        uint32_t held_ms = 0;
#if CONFIG_METER_ACCOUNT_BUTTON_GPIO18
        if (update_button(&next, now, &held_ms)) {
            meter_account_button_action_t action =
                held_ms >= BUTTON_LONG_PRESS_MS ? METER_ACCOUNT_BUTTON_PREVIOUS
                                                : METER_ACCOUNT_BUTTON_NEXT;
            s_callback(action, s_context);
        }
#endif
#if CONFIG_METER_EXTERNAL_PREVIOUS_BUTTON_GPIO0
        if (update_button(&previous, now, &held_ms)) {
            s_callback(METER_ACCOUNT_BUTTON_PREVIOUS, s_context);
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t meter_buttons_start(
    meter_account_button_callback_t callback,
    void *context)
{
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t pins = 0;
#if CONFIG_METER_ACCOUNT_BUTTON_GPIO18
    pins |= 1ULL << ACCOUNT_NEXT_GPIO;
#endif
#if CONFIG_METER_EXTERNAL_PREVIOUS_BUTTON_GPIO0
    pins |= 1ULL << ACCOUNT_PREVIOUS_GPIO;
    ESP_LOGW(TAG, "%s", "External GPIO0 previous button enabled; onboard Key2 also resets CHIP_PU");
#endif
    if (pins == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const gpio_config_t config = {
        .pin_bit_mask = pins,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t result = gpio_config(&config);
    if (result != ESP_OK) {
        return result;
    }

    s_callback = callback;
    s_context = context;
    if (xTaskCreate(buttons_task, "account_buttons", BUTTON_TASK_STACK, NULL,
                    BUTTON_TASK_PRIORITY, &s_task) != pdPASS) {
        s_callback = NULL;
        s_context = NULL;
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_METER_ACCOUNT_BUTTON_GPIO18
    ESP_LOGI(TAG, "%s", "GPIO18 account selector ready: short=next, long=previous");
#endif
    return ESP_OK;
}
