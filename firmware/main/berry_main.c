#include "berry_serial.h"
#include "berry_ui.h"
#include "berry_wifi.h"
#include "berry_panel.h"
#include "berry_button.h"
#include "berry_power.h"
#include "berry_pickup.h"
#include "meter_display.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

volatile int berry_rotation_status=-1;
void app_main(void) {
    esp_err_t nvs = nvs_flash_init();
    if (nvs != ESP_OK) { ESP_LOGE("berry", "NVS unavailable: %s", esp_err_to_name(nvs)); return; }
    vTaskDelay(pdMS_TO_TICKS(250));
    lv_display_t *display = meter_display_start();
    if (!display) { ESP_LOGE("berry", "Display unavailable"); return; }
    bsp_display_lock(-1);
    esp_err_t rotation = meter_display_set_rotation(METER_ROTATION_90);
    berry_rotation_status=(int)rotation;
    if(rotation != ESP_OK) ESP_LOGW("berry", "Display rotation unavailable: %s", esp_err_to_name(rotation));
    berry_power_restore_brightness_locked();
    berry_ui_create(display);
    bsp_display_unlock();
    esp_err_t buttons = berry_button_start();
    if(buttons != ESP_OK) ESP_LOGW("berry", "Button unavailable: %s", esp_err_to_name(buttons));
    berry_panel_start();
    esp_err_t power = berry_power_start();berry_power_start_status=(int)power;
    if(power != ESP_OK) ESP_LOGW("berry", "Power key unavailable: %s", esp_err_to_name(power));
    esp_err_t pickup=berry_pickup_start();
    if(pickup!=ESP_OK)ESP_LOGW("berry","Pickup unavailable: %s",esp_err_to_name(pickup));
    esp_err_t usb = berry_serial_start();
    if (usb != ESP_OK) ESP_LOGE("berry", "USB unavailable: %s", esp_err_to_name(usb));
    esp_err_t wifi = berry_wifi_start(); berry_wifi_start_status=wifi;
    if (wifi != ESP_OK && wifi != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW("berry", "Wi-Fi unavailable: %s", esp_err_to_name(wifi));
}
