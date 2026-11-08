#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "meter_account_selection.h"
#include "meter_battery.h"
#include "meter_buttons.h"
#include "meter_display.h"
#include "meter_orientation.h"
#include "meter_ui.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "transport.h"
#include "usage_model.h"

#define DISPLAY_POWER_SETTLE_MS 250
#define STARTUP_REFRESH_PERIOD_MS 250
#define STARTUP_REFRESH_COUNT 4
#define ROTATION_APPLY_PERIOD_MS 100

static const char *TAG = "usage_meter";

static SemaphoreHandle_t s_snapshot_mutex;
static meter_account_selection_t s_account_selection;
static meter_link_state_t s_transport_link_state = METER_LINK_CONNECTING;
static usage_snapshot_t s_snapshot = {
    .valid = false,
    .link_state = METER_LINK_CONNECTING,
    .resets_in = -1,
    .extra_remaining = -1,
    .extra_resets_in = -1,
    .next_poll_in = 60,
    .plan = "",
    .extra_name = "ADDITIONAL LIMIT",
};
static meter_battery_snapshot_t s_battery = {
    .valid = false,
    .percent = -1,
};

static void snapshot_copy(
    usage_snapshot_t *target,
    meter_battery_snapshot_t *battery)
{
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    *target = s_snapshot;
    *battery = s_battery;
    xSemaphoreGive(s_snapshot_mutex);
}

static void on_transport_snapshot(const usage_snapshot_t *snapshot, void *context)
{
    (void)context;
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    s_transport_link_state = snapshot->link_state;
    s_snapshot = *snapshot;
    meter_account_selection_reconcile(&s_account_selection, &s_snapshot);
    meter_account_selection_apply_transport_link(
        &s_snapshot, s_transport_link_state);
    xSemaphoreGive(s_snapshot_mutex);
}

static void on_transport_link(meter_link_state_t state, void *context)
{
    (void)context;
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    s_transport_link_state = state;
    meter_account_selection_apply_transport_link(
        &s_snapshot, s_transport_link_state);
    xSemaphoreGive(s_snapshot_mutex);
}

static void on_transport_setup(const char *ssid, const char *password, void *context)
{
    (void)context;
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    s_transport_link_state = METER_LINK_SETUP;
    s_snapshot.valid = false;
    s_snapshot.link_state = METER_LINK_SETUP;
    s_snapshot.account_count = 0;
    s_snapshot.selected_account = 0;
    snprintf(s_snapshot.setup_ssid, sizeof(s_snapshot.setup_ssid), "%s", ssid ? ssid : "");
    snprintf(s_snapshot.setup_psk, sizeof(s_snapshot.setup_psk), "%s",
             password ? password : "");
    xSemaphoreGive(s_snapshot_mutex);
}

#if CONFIG_METER_ACCOUNT_BUTTON_GPIO18 || CONFIG_METER_EXTERNAL_PREVIOUS_BUTTON_GPIO0
static void on_account_button(meter_account_button_action_t action, void *context)
{
    (void)context;
    unsigned selected = 0;
    unsigned count = 0;

    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    bool changed = meter_account_selection_step(
        &s_account_selection, &s_snapshot,
        action == METER_ACCOUNT_BUTTON_PREVIOUS ? -1 : 1);
    if (changed) {
        meter_account_selection_apply_transport_link(
            &s_snapshot, s_transport_link_state);
        selected = (unsigned)s_snapshot.selected_account + 1U;
        count = s_snapshot.account_count;
    }
    xSemaphoreGive(s_snapshot_mutex);

    if (changed) {
        /* The display already presents the requested identity. Keep email
         * addresses and aliases out of persistent serial logs. */
        ESP_LOGI(TAG, "Selected account %u/%u", selected, count);
    }
}
#endif

static void on_battery_snapshot(
    const meter_battery_snapshot_t *snapshot,
    void *context)
{
    (void)context;
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
    s_battery = *snapshot;
    xSemaphoreGive(s_snapshot_mutex);
}

static void render_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    usage_snapshot_t current;
    meter_battery_snapshot_t battery;
    snapshot_copy(&current, &battery);
    meter_ui_render(&current, &battery);
}

static void startup_refresh_timer_cb(lv_timer_t *timer)
{
    lv_display_t *display = lv_timer_get_user_data(timer);
    lv_obj_t *screen = lv_display_get_screen_active(display);
    if (screen) {
        lv_obj_invalidate(screen);
    }
}

#if CONFIG_METER_TRANSPORT_BLE
static void setup_recovery_task(void *argument)
{
    (void)argument;
    esp_err_t result = meter_transport_request_setup();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Unable to open physical recovery portal: %s",
                 esp_err_to_name(result));
    }
    vTaskDelete(NULL);
}

static void setup_recovery_requested(void *context)
{
    (void)context;
    if (xTaskCreate(setup_recovery_task, "setup_recovery", 6144, NULL, 5, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "Unable to create setup recovery task");
    }
}
#endif

#if CONFIG_METER_AUTO_ROTATE
static void rotation_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    meter_rotation_t rotation;
    if (meter_orientation_get_rotation(&rotation)) {
        (void)meter_display_set_rotation(rotation);
    }
}
#endif

#if CONFIG_METER_MOTION_WAKE
static uint32_t s_last_motion_generation;

static bool motion_activity_query(void *context)
{
    (void)context;
    uint32_t generation;
    if (!meter_orientation_get_motion_generation(&generation) ||
        generation == s_last_motion_generation) {
        return false;
    }

    s_last_motion_generation = generation;
    return true;
}
#endif

void app_main(void)
{
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);

    s_snapshot_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_snapshot_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    meter_account_selection_init(&s_account_selection);

    vTaskDelay(pdMS_TO_TICKS(DISPLAY_POWER_SETTLE_MS));
    lv_display_t *display = meter_display_start();
    ESP_ERROR_CHECK(display ? ESP_OK : ESP_FAIL);

    bsp_display_lock(-1);
#if CONFIG_METER_MOTION_WAKE
    meter_ui_set_motion_query(motion_activity_query, NULL);
#endif
    meter_ui_create(display);
#if CONFIG_METER_TRANSPORT_BLE
    meter_ui_set_recovery_callback(setup_recovery_requested, NULL);
    ESP_ERROR_CHECK(meter_display_set_touch_long_press_time(
        CONFIG_METER_BLE_SETUP_HOLD_SECONDS * 1000U));
#endif
    lv_timer_create(render_timer_cb, 1000, NULL);
    render_timer_cb(NULL);
    lv_obj_invalidate(lv_display_get_screen_active(display));
    lv_refr_now(display);

    lv_timer_t *startup_refresh =
        lv_timer_create(startup_refresh_timer_cb, STARTUP_REFRESH_PERIOD_MS, display);
    ESP_ERROR_CHECK(startup_refresh ? ESP_OK : ESP_ERR_NO_MEM);
    lv_timer_set_repeat_count(startup_refresh, STARTUP_REFRESH_COUNT);
#if CONFIG_METER_AUTO_ROTATE
    lv_timer_t *rotation_timer =
        lv_timer_create(rotation_timer_cb, ROTATION_APPLY_PERIOD_MS, NULL);
    ESP_ERROR_CHECK(rotation_timer ? ESP_OK : ESP_ERR_NO_MEM);
#endif
    bsp_display_unlock();

    vTaskDelay(pdMS_TO_TICKS(STARTUP_REFRESH_PERIOD_MS + 100));
    ESP_ERROR_CHECK(bsp_display_brightness_set(CONFIG_METER_BRIGHTNESS));

#if CONFIG_METER_AUTO_ROTATE || CONFIG_METER_MOTION_WAKE
    esp_err_t orientation_result = meter_orientation_start();
    if (orientation_result != ESP_OK) {
        ESP_LOGW(TAG,
                 "QMI8658 activity service unavailable; sensor-driven display features are disabled: %s",
                 esp_err_to_name(orientation_result));
    }
#endif

    esp_err_t battery_result = meter_battery_start(on_battery_snapshot, NULL);
    if (battery_result != ESP_OK) {
        ESP_LOGW(
            TAG, "Battery monitor unavailable; quota display continues: %s",
            esp_err_to_name(battery_result));
    }

#if CONFIG_METER_ACCOUNT_BUTTON_GPIO18 || CONFIG_METER_EXTERNAL_PREVIOUS_BUTTON_GPIO0
    esp_err_t buttons_result = meter_buttons_start(on_account_button, NULL);
    if (buttons_result != ESP_OK) {
        ESP_LOGW(TAG, "Account buttons unavailable: %s",
                 esp_err_to_name(buttons_result));
    }
#endif

    const meter_transport_callbacks_t callbacks = {
        .on_snapshot = on_transport_snapshot,
        .on_link = on_transport_link,
        .on_setup = on_transport_setup,
        .context = NULL,
    };
    esp_err_t transport_result = meter_transport_start(&callbacks);
    if (transport_result != ESP_OK && transport_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Transport start failed: %s", esp_err_to_name(transport_result));
        on_transport_link(METER_LINK_OFFLINE, NULL);
    }
}
