#include "meter_display.h"
#include "berry_power.h"
#include "berry_touch_transform.h"

#include "esp_err.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_display.h"
#include "esp_lv_adapter_input.h"

#define METER_DRAW_BUFFER_LINES 10

static const char *TAG = "meter_display";
static lv_display_t *s_display;
static esp_lcd_touch_handle_t s_touch;
static lv_indev_t *s_touch_input;
static meter_rotation_t s_rotation = METER_ROTATION_INVALID;

static esp_err_t touch_read_rotated(
    esp_lcd_touch_handle_t touch,
    esp_lcd_touch_point_data_t *points,
    uint8_t *count,
    uint8_t max_count,
    void *context)
{
    (void)context;
    esp_err_t result = esp_lcd_touch_read_data(touch);
    if (result != ESP_OK) {
        return result;
    }

    result = esp_lcd_touch_get_data(touch, points, count, max_count);
    if (result != ESP_OK) {
        return result;
    }

    if(berry_power_screen_off()){*count=0;return ESP_OK;}
    for (uint8_t index = 0; index < *count; ++index) {
        uint16_t x=points[index].x,y=points[index].y;
        berry_touch_transform(s_rotation,BSP_LCD_H_RES,BSP_LCD_V_RES,&x,&y);
        points[index].x=x;points[index].y=y;
    }
    return ESP_OK;
}

static bool rotation_to_bsp(
    meter_rotation_t rotation,
    bsp_display_rotation_t *bsp_rotation)
{
    if (!bsp_rotation) {
        return false;
    }

    /*
     * On this panel/BSP combination the quarter-turn command names are the
     * inverse of the physical picture direction observed on the assembled
     * board. Keep the rest of the application in physical display coordinates.
     */
    switch (rotation) {
    case METER_ROTATION_0:
        *bsp_rotation = BSP_DISPLAY_ROTATE_0;
        return true;
    case METER_ROTATION_90:
        *bsp_rotation = BSP_DISPLAY_ROTATE_270;
        return true;
    case METER_ROTATION_180:
        *bsp_rotation = BSP_DISPLAY_ROTATE_180;
        return true;
    case METER_ROTATION_270:
        *bsp_rotation = BSP_DISPLAY_ROTATE_90;
        return true;
    default:
        return false;
    }
}

static void rounder_event_cb(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_param(event);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

lv_display_t *meter_display_start(void)
{
    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    esp_err_t result = esp_lv_adapter_init(&adapter_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "LVGL adapter init failed: %s", esp_err_to_name(result));
        return NULL;
    }

    const bsp_display_config_t panel_config = {
        .max_transfer_sz =
            BSP_LCD_H_RES * METER_DRAW_BUFFER_LINES * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t panel_io = NULL;
    result = bsp_display_new(&panel_config, &panel, &panel_io);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Panel init failed: %s", esp_err_to_name(result));
        return NULL;
    }

    result = bsp_display_rotation_set(BSP_DISPLAY_ROTATE_0);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Hardware rotation setup failed: %s", esp_err_to_name(result));
        return NULL;
    }

    result = bsp_display_brightness_set(0);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not blank panel: %s", esp_err_to_name(result));
        return NULL;
    }

    esp_lv_adapter_display_config_t display_config =
        ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(
            panel, panel_io, BSP_LCD_H_RES, BSP_LCD_V_RES, ESP_LV_ADAPTER_ROTATE_0);
    display_config.profile.buffer_height = METER_DRAW_BUFFER_LINES;
    display_config.profile.use_psram = false;
    display_config.profile.require_double_buffer = false;
    lv_display_t *display = esp_lv_adapter_register_display(&display_config);
    if (!display) {
        ESP_LOGE(TAG, "Display registration failed");
        return NULL;
    }
    lv_display_add_event_cb(display, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    const bsp_display_cfg_t touch_board_config = {
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    result = bsp_touch_new(&touch_board_config, &s_touch);
    if (result != ESP_OK || !s_touch) {
        ESP_LOGE(TAG, "Touch init failed: %s", esp_err_to_name(result));
        return NULL;
    }

    esp_lv_adapter_touch_config_t touch_config =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, s_touch);
    touch_config.callbacks.custom_touch_read = touch_read_rotated;
    s_touch_input = esp_lv_adapter_register_touch(&touch_config);
    if (!s_touch_input) {
        ESP_LOGE(TAG, "Touch registration failed");
        return NULL;
    }

    result = esp_lv_adapter_start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "LVGL task start failed: %s", esp_err_to_name(result));
        return NULL;
    }

    s_display = display;
    /*
     * Hardware rotation 0 is only a deterministic initialization baseline.
     * The first stable QMI8658 result must still be applied.
     */
    s_rotation = METER_ROTATION_INVALID;
    ESP_LOGI(TAG, "Using %d-line internal-RAM draw buffer", METER_DRAW_BUFFER_LINES);
    return display;
}

esp_err_t meter_display_set_touch_long_press_time(uint32_t milliseconds)
{
    if (milliseconds == 0 || milliseconds > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_touch_input) {
        return ESP_ERR_INVALID_STATE;
    }

    lv_indev_set_long_press_time(s_touch_input, (uint16_t)milliseconds);
    return ESP_OK;
}

esp_err_t meter_display_set_rotation(meter_rotation_t rotation)
{
    bsp_display_rotation_t bsp_rotation;
    if (!rotation_to_bsp(rotation, &bsp_rotation)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_display || !s_touch || !s_touch_input) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rotation == s_rotation) {
        return ESP_OK;
    }
    if (lv_indev_get_state(s_touch_input) == LV_INDEV_STATE_PRESSED) {
        return ESP_ERR_INVALID_STATE;
    }

    const meter_rotation_t previous = s_rotation;
    esp_err_t result = bsp_display_rotation_set(bsp_rotation);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not rotate display to %d degrees: %s",
                 (int)rotation * 90, esp_err_to_name(result));
        return result;
    }

    s_rotation = rotation;
    lv_obj_t *screen = lv_display_get_screen_active(s_display);
    if (screen) {
        lv_obj_invalidate(screen);
        lv_refr_now(s_display);
    }
    if (previous == METER_ROTATION_INVALID) {
        ESP_LOGI(TAG, "Display and touch rotation initialized to %d degrees",
                 (int)rotation * 90);
    } else {
        ESP_LOGI(TAG, "Display and touch rotation changed from %d to %d degrees",
                 (int)previous * 90, (int)rotation * 90);
    }
    return ESP_OK;
}
